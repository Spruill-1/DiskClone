# End-to-end clone test against scratch VHDX disks — no physical disk is ever
# touched. For each partition style (GPT, MBR):
#   1. Create + attach a 2 GiB source and 4 GiB target VHDX via inbox diskpart.
#   2. Partition the source (GPT: MSR + NTFS data; MBR: active NTFS primary),
#      write random test files, and record their SHA-256 hashes.
#   3. Run `diskclone clone --expand --force` between the two virtual disks.
#   4. Verify: identity preservation (GPT disk GUID / MBR signature + sector-0
#      bootstrap), partition table equality, data-partition expansion, file
#      hashes on the mounted clone, the deferred `extend filesystem` step, and
#      a clean chkdsk.
#
# Requires elevation (the tool itself demands it); GitHub Actions Windows
# runners already run elevated. Exit code 0 = all assertions passed.

param(
    [string]$ExePath = (Join-Path $PSScriptRoot '..\x64\Release\diskclone.exe'),
    [string]$WorkDir = (Join-Path $env:TEMP 'diskclone-vhdtest')
)

$ErrorActionPreference = 'Stop'
$ExePath = (Resolve-Path $ExePath).Path
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$script:fail = $false

function Check($name, $condition) {
    if ($condition) { Write-Output "PASS: $name" } else { Write-Output "FAIL: $name"; $script:fail = $true }
}

function Read-Sector0($diskNumber) {
    $stream = New-Object System.IO.FileStream("\\.\PHYSICALDRIVE$diskNumber", 'Open', 'Read', 'ReadWrite')
    try { $buffer = New-Object byte[] 512; [void]$stream.Read($buffer, 0, 512); return $buffer }
    finally { $stream.Dispose() }
}

# Path formats in Get-Disk's Location vary across environments (8.3 segments,
# casing), and disk arrival can lag the diskpart attach — so match virtual
# disks by unique filename, case-insensitively, with a retry window.
function Find-VhdDisk([string]$VhdPath) {
    $leafName = Split-Path $VhdPath -Leaf
    for ($attempt = 0; $attempt -lt 15; $attempt++) {
        $disk = Get-Disk | Where-Object {
            $_.BusType -eq 'File Backed Virtual' -and $_.Location -and
            ((Split-Path $_.Location -Leaf) -ieq $leafName)
        }
        if ($disk) { return $disk }
        Start-Sleep -Seconds 1
    }

    return $null
}

function Invoke-CloneRound([string]$Style) {
    Write-Output "===== ROUND: $Style ====="
    $sourceVhd = Join-Path $WorkDir "clonesrc-$Style.vhdx"
    $targetVhd = Join-Path $WorkDir "clonetgt-$Style.vhdx"
    try {
        foreach ($vhd in @($sourceVhd, $targetVhd)) {
            if (Test-Path $vhd) {
                "select vdisk file=`"$vhd`"`ndetach vdisk" | diskpart | Out-Null
                Remove-Item $vhd -Force -ErrorAction SilentlyContinue
            }
        }
        $diskpartOutput = @"
create vdisk file="$sourceVhd" maximum=2048 type=expandable
attach vdisk
create vdisk file="$targetVhd" maximum=4096 type=expandable
attach vdisk
"@ | diskpart

        $sourceDisk = Find-VhdDisk $sourceVhd
        $targetDisk = Find-VhdDisk $targetVhd
        if (-not $sourceDisk -or -not $targetDisk) {
            Write-Output "diskpart output was:"
            $diskpartOutput | ForEach-Object { "  $_" }
            Get-Disk | Format-Table Number, BusType, Size, Location -AutoSize | Out-String | Write-Output
            throw "could not find attached VHD disks"
        }
        if ([math]::Abs($sourceDisk.Size - 2GB) -gt 200MB -or [math]::Abs($targetDisk.Size - 4GB) -gt 200MB) { throw "size sanity check failed - aborting" }
        Write-Output "source disk: $($sourceDisk.Number)  target disk: $($targetDisk.Number)"

        Initialize-Disk -Number $sourceDisk.Number -PartitionStyle $Style
        if ($Style -eq 'GPT') {
            New-Partition -DiskNumber $sourceDisk.Number -GptType '{e3c9e316-0b5c-4db8-817d-f92df00215ae}' -Size 16MB | Out-Null
            $partition = New-Partition -DiskNumber $sourceDisk.Number -UseMaximumSize -AssignDriveLetter
        } else {
            $partition = New-Partition -DiskNumber $sourceDisk.Number -UseMaximumSize -IsActive -AssignDriveLetter
        }
        $volume = Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel "SRC$Style" -Confirm:$false
        $sourceLetter = $volume.DriveLetter
        Write-Output "source volume: ${sourceLetter}:"

        $random = [System.Random]::new(42)
        New-Item -ItemType Directory -Path "${sourceLetter}:\data" | Out-Null
        foreach ($i in 1..5) {
            $bytes = New-Object byte[] (10MB)
            $random.NextBytes($bytes)
            [System.IO.File]::WriteAllBytes("${sourceLetter}:\data\file$i.bin", $bytes)
        }
        $sourceHashes = Get-ChildItem "${sourceLetter}:\data" | Get-FileHash -Algorithm SHA256 | Sort-Object Path
        try { Write-VolumeCache -DriveLetter $sourceLetter } catch { }

        $sourceParts = Get-Partition -DiskNumber $sourceDisk.Number | Sort-Object Offset |
            Select-Object Offset, Size, Type, GptType, Guid, IsActive, MbrType
        $sourceDiskInfo = Get-Disk -Number $sourceDisk.Number

        Write-Output "--- running diskclone ($Style) ---"
        cmd /c "`"$ExePath`" clone --source $($sourceDisk.Number) --target $($targetDisk.Number) --expand --force 2>&1" | ForEach-Object { "$_" }
        $exitCode = $LASTEXITCODE
        Write-Output "--- diskclone exit: $exitCode ---"
        Check "[$Style] clone exit code 0" ($exitCode -eq 0)
        if ($exitCode -ne 0) { throw "clone failed" }

        if ($Style -eq 'MBR') {
            $sourceSector = Read-Sector0 $sourceDisk.Number
            $targetSector = Read-Sector0 $targetDisk.Number
            $bootstrapEqual = $true
            for ($i = 0; $i -lt 440; $i++) { if ($sourceSector[$i] -ne $targetSector[$i]) { $bootstrapEqual = $false; break } }
            Check "[MBR] 440-byte bootstrap spliced" $bootstrapEqual
            $signatureEqual = $true
            for ($i = 440; $i -lt 444; $i++) { if ($sourceSector[$i] -ne $targetSector[$i]) { $signatureEqual = $false; break } }
            Check "[MBR] disk signature preserved in sector 0" $signatureEqual
            Check "[MBR] boot record 55AA" ($targetSector[510] -eq 0x55 -and $targetSector[511] -eq 0xAA)
        }

        "select vdisk file=`"$sourceVhd`"`ndetach vdisk" | diskpart | Out-Null
        Start-Sleep -Seconds 1
        Set-Disk -Number $targetDisk.Number -IsOffline $false
        Start-Sleep -Seconds 2

        $targetDiskInfo = Get-Disk -Number $targetDisk.Number
        if ($Style -eq 'GPT') {
            Check "[GPT] disk GUID preserved" ($targetDiskInfo.Guid -eq $sourceDiskInfo.Guid)
        } else {
            Check "[MBR] disk signature preserved (Get-Disk)" ($targetDiskInfo.Signature -eq $sourceDiskInfo.Signature)
        }
        $targetParts = Get-Partition -DiskNumber $targetDisk.Number | Sort-Object Offset |
            Select-Object Offset, Size, Type, GptType, Guid, IsActive, MbrType
        Check "[$Style] partition count preserved" (@($targetParts).Count -eq @($sourceParts).Count)
        for ($i = 0; $i -lt @($sourceParts).Count; $i++) {
            $src = @($sourceParts)[$i]; $tgt = @($targetParts)[$i]
            if ($Style -eq 'GPT') {
                Check "[$Style] partition $i offset+type+guid" (
                    $tgt.Offset -eq $src.Offset -and "$($tgt.GptType)" -eq "$($src.GptType)" -and "$($tgt.Guid)" -eq "$($src.Guid)")
            } else {
                Check "[$Style] partition $i offset+type+active" (
                    $tgt.Offset -eq $src.Offset -and $tgt.MbrType -eq $src.MbrType -and $tgt.IsActive -eq $src.IsActive)
            }
        }
        $lastSource = @($sourceParts)[-1]; $lastTarget = @($targetParts)[-1]
        Check "[$Style] data partition expanded" ($lastTarget.Size -gt $lastSource.Size)
        Write-Output "data partition: $([math]::Round($lastSource.Size/1MB))MB -> $([math]::Round($lastTarget.Size/1MB))MB"

        $targetVolume = Get-Partition -DiskNumber $targetDisk.Number | Get-Volume | Where-Object FileSystemLabel -eq "SRC$Style"
        if (-not $targetVolume.DriveLetter) {
            $lastPartition = Get-Partition -DiskNumber $targetDisk.Number | Sort-Object Offset | Select-Object -Last 1
            $lastPartition | Add-PartitionAccessPath -AssignDriveLetter
            $targetVolume = Get-Partition -DiskNumber $targetDisk.Number | Get-Volume | Where-Object FileSystemLabel -eq "SRC$Style"
        }
        $targetLetter = $targetVolume.DriveLetter
        Write-Output "clone volume: ${targetLetter}:"
        Check "[$Style] clone volume mounted" ($null -ne $targetLetter)

        $targetHashes = Get-ChildItem "${targetLetter}:\data" | Get-FileHash -Algorithm SHA256 | Sort-Object Path
        Check "[$Style] file count matches" (@($targetHashes).Count -eq @($sourceHashes).Count)
        for ($i = 0; $i -lt @($sourceHashes).Count; $i++) {
            Check "[$Style] hash file$($i+1)" (@($targetHashes)[$i].Hash -eq @($sourceHashes)[$i].Hash)
        }

        $volumeSizeBefore = (Get-Volume -DriveLetter $targetLetter).Size
        "select volume $targetLetter`nextend filesystem" | diskpart | Out-Null
        $volumeSizeAfter = (Get-Volume -DriveLetter $targetLetter).Size
        Write-Output "volume size: $([math]::Round($volumeSizeBefore/1MB))MB -> $([math]::Round($volumeSizeAfter/1MB))MB"
        Check "[$Style] extend filesystem grew the volume" ($volumeSizeAfter -gt $volumeSizeBefore)

        $chkdskOutput = cmd /c "chkdsk ${targetLetter}: /scan 2>&1"
        Check "[$Style] chkdsk found no problems" (($chkdskOutput -join ' ') -match 'found no problems')
    }
    catch {
        Write-Output "TEST ERROR [$Style]: $($_.Exception.Message)"
        $script:fail = $true
    }
    finally {
        foreach ($vhd in @($sourceVhd, $targetVhd)) {
            try { "select vdisk file=`"$vhd`"`ndetach vdisk" | diskpart | Out-Null } catch { }
            try { Remove-Item $vhd -Force -ErrorAction SilentlyContinue } catch { }
        }
    }
}

Invoke-CloneRound 'GPT'
Invoke-CloneRound 'MBR'
if ($script:fail) { Write-Output 'RESULT: FAIL'; exit 1 } else { Write-Output 'RESULT: PASS'; exit 0 }
