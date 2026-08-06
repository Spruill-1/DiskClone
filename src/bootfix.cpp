#include "bootfix.h"
#include "volumes.h"

#include <winioctl.h>

#include <algorithm>

namespace DiskClone
{
    void SpliceMbrBootstrap(HANDLE sourceDisk, HANDLE targetDisk, uint32_t sectorSize)
    {
        auto sourceSector = AllocateAlignedBuffer(std::max<uint32_t>(sectorSize, 4096));
        auto targetSector = AllocateAlignedBuffer(std::max<uint32_t>(sectorSize, 4096));
        OVERLAPPED overlappedSource{};
        DWORD bytesTransferred = 0;
        if (!ReadFile(sourceDisk, sourceSector.get(), sectorSize, &bytesTransferred, &overlappedSource) ||
            bytesTransferred != sectorSize)
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"failed to read source MBR", lastError);
        }

        OVERLAPPED overlappedTarget{};
        if (!ReadFile(targetDisk, targetSector.get(), sectorSize, &bytesTransferred, &overlappedTarget) ||
            bytesTransferred != sectorSize)
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"failed to read target MBR", lastError);
        }

        memcpy(targetSector.get(), sourceSector.get(), 440);        // bootstrap only; keep sig+table+55AA
        OVERLAPPED overlappedWrite{};
        if (!WriteFile(targetDisk, targetSector.get(), sectorSize, &bytesTransferred, &overlappedWrite) ||
            bytesTransferred != sectorSize)
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"failed to write target MBR", lastError);
        }
    }

    // -----------------------------------------------------------------------
    // --new-ids finalization
    // -----------------------------------------------------------------------

    namespace
    {
        [[noreturn]] void FinalizeFail(const std::wstring& why, const std::wstring& recovery)
        {
            throw Error(ExitCode::FinalizeFailure,
                why + L"\nThe data copy itself succeeded. Finish manually:\n" + recovery);
        }

        // Waits for the volume backing (diskNumber, partitionOffset) to appear.
        std::wstring WaitForVolume(int diskNumber, uint64_t partitionOffset, DWORD timeoutMs)
        {
            ULONGLONG startTick = GetTickCount64();
            for (;;)
            {
                for (const auto& volume : EnumerateVolumes())
                {
                    if (volume.diskNumber == diskNumber && volume.offset == partitionOffset)
                    {
                        return volume.guidPath;
                    }
                }

                if (GetTickCount64() - startTick > timeoutMs) { return L""; }
                Sleep(500);
            }
        }

        // Removes its mount point on destruction — unless dismissed (mountPoint
        // cleared), which the failure paths use to keep recovery commands valid.
        // Intentionally custom: no upstream helper offers dismissible,
        // delayed-arm cleanup keyed on a value assigned after construction.
        struct TempMount
        {
            std::wstring mountPoint;   // "W:\\"
            ~TempMount()
            {
                if (!mountPoint.empty()) { DeleteVolumeMountPointW(mountPoint.c_str()); }
            }
        };

        wchar_t FindFreeDriveLetter()
        {
            DWORD driveMask = GetLogicalDrives();
            for (wchar_t letter = L'Z'; letter >= L'G'; --letter)
            {
                if (!(driveMask & (1u << (letter - L'A')))) { return letter; }
            }

            return 0;
        }

        bool MountAt(const std::wstring& volumeGuidPath, wchar_t letter, TempMount& mount)
        {
            std::wstring mountPoint = std::wstring(1, letter) + L":\\";
            if (!SetVolumeMountPointW(mountPoint.c_str(), volumeGuidPath.c_str())) { return false; }
            mount.mountPoint = mountPoint;
            return true;
        }

        int RunProcess(const std::wstring& commandLine)
        {
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            wil::unique_process_information processInfo;
            std::wstring mutableCommand = commandLine;   // CreateProcessW needs a writable buffer
            if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                    nullptr, nullptr, &startupInfo, &processInfo))
            {
                return -1;
            }

            WaitForSingleObject(processInfo.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(processInfo.hProcess, &exitCode);
            return static_cast<int>(exitCode);
        }

        std::wstring System32Path(const wchar_t* exeName)
        {
            wchar_t systemDirectory[MAX_PATH];
            GetSystemDirectoryW(systemDirectory, MAX_PATH);
            return std::wstring(systemDirectory) + L"\\" + exeName;
        }

        // With fresh partition GUIDs (and a fresh MBR signature), the clone's
        // HKLM\SYSTEM\MountedDevices still maps \DosDevices\C: to the OLD
        // identity; at first boot mountmgr would assign the boot volume a
        // different letter and the system breaks. Standard fix: clear
        // MountedDevices in the clone's own SYSTEM hive so mountmgr
        // regenerates it at boot. Requires SeBackup/SeRestore (enabled at
        // startup) for RegLoadKey.
        void ResetCloneMountedDevices(wchar_t windowsLetter)
        {
            const wchar_t* loadName = L"diskclone-clone-SYSTEM";
            std::wstring hivePath = std::wstring(1, windowsLetter) + L":\\Windows\\System32\\config\\SYSTEM";
            LSTATUS status = RegLoadKeyW(HKEY_LOCAL_MACHINE, loadName, hivePath.c_str());
            if (status != ERROR_SUCCESS)
            {
                FinalizeFail(L"cannot load the clone's SYSTEM hive to reset MountedDevices (error " +
                        std::to_wstring(status) + L")",
                    L"  On the clone's first boot, if drive letters are wrong: boot WinRE,\n"
                    L"  load the clone's SYSTEM hive in regedit and delete the values under\n"
                    L"  MountedDevices, then reboot.");
            }

            auto hiveUnloader = wil::scope_exit([&] { RegUnLoadKeyW(HKEY_LOCAL_MACHINE, loadName); });

            wil::unique_hkey mountedDevicesKey;
            std::wstring subkeyPath = std::wstring(loadName) + L"\\MountedDevices";
            status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkeyPath.c_str(), 0,
                KEY_QUERY_VALUE | KEY_SET_VALUE, &mountedDevicesKey);
            if (status == ERROR_FILE_NOT_FOUND) { return; }   // nothing recorded; mountmgr will populate
            if (status != ERROR_SUCCESS)
            {
                FinalizeFail(L"cannot open the clone's MountedDevices key (error " + std::to_wstring(status) + L")",
                    L"  Delete the values under MountedDevices in the clone's SYSTEM hive manually.");
            }

            for (;;)
            {
                wchar_t valueName[512];
                DWORD valueNameLength = ARRAYSIZE(valueName);
                status = RegEnumValueW(mountedDevicesKey.get(), 0, valueName, &valueNameLength,
                    nullptr, nullptr, nullptr, nullptr);
                if (status != ERROR_SUCCESS) { break; }
                if (RegDeleteValueW(mountedDevicesKey.get(), valueName) != ERROR_SUCCESS) { break; }
            }
        }
    }

    void FinalizeNewIds(const ClonePlan& plan, HANDLE targetDisk)
    {
        const bool isGpt = plan.sourceDisk.style == PARTITION_STYLE_GPT;

        // The Windows partition was identified ONCE during planning (layout.cpp
        // refuses --new-ids when it cannot be identified); no second heuristic here.
        const PlannedPartition* windowsPartition = nullptr;
        const PlannedPartition* bootPartition = nullptr;
        for (const auto& planned : plan.parts)
        {
            if (planned.isWindowsPartition) { windowsPartition = &planned; }
            if (isGpt && IsEqualGUID(planned.src.gptType, kEspType)) { bootPartition = &planned; }
            if (!isGpt && planned.src.mbrActive) { bootPartition = &planned; }
        }

        if (!windowsPartition)
        {
            FinalizeFail(L"internal error: no Windows partition was flagged in the plan",
                L"  1. Online the disk in Disk Management\n"
                L"  2. bcdboot <clone>:\\Windows /s <esp>: /f " + std::wstring(isGpt ? L"UEFI" : L"BIOS"));
        }

        // BIOS boot files may live on the Windows partition itself when no
        // partition is marked active; GPT strictly requires the ESP.
        if (isGpt && !bootPartition)
        {
            FinalizeFail(L"the clone has no EFI System Partition; cannot repair boot files",
                L"  bcdboot <clone>:\\Windows /s <esp>: /f UEFI");
        }

        // 1. Bring the target online.
        try
        {
            SetDiskOffline(targetDisk, false);
        }
        catch (const Error& e)
        {
            // Data copy succeeded; onlining is the only thing that failed.
            FinalizeFail(L"could not bring the target disk online (" + e.Message() + L")",
                L"  1. Disk Management: right-click disk " + std::to_wstring(plan.targetDisk.number) +
                L" -> Online\n"
                L"  2. bcdboot <clone>:\\Windows /s <esp>: /f " + std::wstring(isGpt ? L"UEFI" : L"BIOS"));
        }

        UpdateDiskProperties(targetDisk);

        // 2. Wait for volumes to arrive.
        std::wstring windowsVolume = WaitForVolume(plan.targetDisk.number, windowsPartition->targetOffset, 15000);
        if (windowsVolume.empty())
        {
            FinalizeFail(L"the clone's Windows volume did not come online",
                L"  bcdboot <clone>:\\Windows /s <esp>: /f " + std::wstring(isGpt ? L"UEFI" : L"BIOS"));
        }

        // 3. Temp mounts.
        TempMount windowsMount, espMount;
        wchar_t windowsLetter = FindFreeDriveLetter();
        if (!windowsLetter || !MountAt(windowsVolume, windowsLetter, windowsMount))
        {
            FinalizeFail(L"cannot assign a temp drive letter to the clone's Windows volume",
                L"  bcdboot <clone>:\\Windows /s <esp>: /f " + std::wstring(isGpt ? L"UEFI" : L"BIOS"));
        }

        // bcdboot MUST always receive /s: without it, it writes to the RUNNING
        // system's own boot partition — corrupting this machine's boot config
        // to point at a temporary drive letter. If the clone's boot partition
        // cannot be mounted, fail instead of falling through.
        std::wstring bootSwitch;
        const PlannedPartition* bootFilesTarget = bootPartition ? bootPartition : windowsPartition;   // BIOS: boot files can live on the Windows partition
        {
            std::wstring bootVolume = WaitForVolume(plan.targetDisk.number, bootFilesTarget->targetOffset, 15000);
            if (bootFilesTarget == windowsPartition)
            {
                bootSwitch = L" /s " + std::wstring(1, windowsLetter) + L":";
            }
            else if (bootVolume.empty())
            {
                FinalizeFail(L"the clone's boot partition (ESP/system) did not come online",
                    L"  bcdboot " + std::wstring(1, windowsLetter) + L":\\Windows /s <esp>: /f " +
                    std::wstring(isGpt ? L"UEFI" : L"BIOS"));
            }
            else
            {
                wchar_t bootLetter = 0;
                DWORD driveMask = GetLogicalDrives();
                for (wchar_t letter = L'Z'; letter >= L'G'; --letter)
                {
                    if (letter != windowsLetter && !(driveMask & (1u << (letter - L'A'))))
                    {
                        bootLetter = letter;
                        break;
                    }
                }

                if (!bootLetter || !MountAt(bootVolume, bootLetter, espMount))
                {
                    FinalizeFail(L"cannot assign a temp drive letter to the clone's boot partition",
                        L"  bcdboot " + std::wstring(1, windowsLetter) + L":\\Windows /s <esp>: /f " +
                        std::wstring(isGpt ? L"UEFI" : L"BIOS"));
                }

                bootSwitch = L" /s " + std::wstring(1, bootLetter) + L":";
            }
        }

        // Steps 4-6: on any failure below, KEEP the temp drive letters mounted
        // so the printed recovery commands (which reference them) remain valid.
        try
        {
            // 4. bcdboot — bootSwitch is guaranteed non-empty by construction above.
            std::wstring command = System32Path(L"bcdboot.exe") + L" " +
                std::wstring(1, windowsLetter) + L":\\Windows" +
                bootSwitch + L" /f " + (isGpt ? L"UEFI" : L"BIOS");
            fwprintf(stderr, L"Running: %s\n", command.c_str());
            int exitCode = RunProcess(command);
            if (exitCode != 0)
            {
                FinalizeFail(L"bcdboot failed with exit code " + std::to_wstring(exitCode),
                    L"  " + command);
            }

            // 5. Reset the clone's MountedDevices so its fresh partition IDs
            //    get re-mapped at first boot (drive-letter preservation).
            ResetCloneMountedDevices(windowsLetter);

            // 6. Extend the NTFS filesystem if --expand grew the Windows partition.
            if (plan.opts.expand && windowsPartition->targetLength > windowsPartition->src.length)
            {
                std::wstring volumeNoSlash = windowsVolume;
                if (!volumeNoSlash.empty() && volumeNoSlash.back() == L'\\') { volumeNoSlash.pop_back(); }
                wil::unique_hfile volume{ CreateFileW(volumeNoSlash.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr) };
                if (!volume)
                {
                    FinalizeFail(L"cannot open the clone's Windows volume to extend it",
                        L"  diskpart:  select volume " + std::wstring(1, windowsLetter) + L"  /  extend filesystem");
                }

                LONGLONG newSectorCount = static_cast<LONGLONG>(
                    windowsPartition->targetLength / plan.sourceDisk.logicalSectorSize);
                DWORD bytesReturned = 0;
                if (!DeviceIoControl(volume.get(), FSCTL_EXTEND_VOLUME, &newSectorCount, sizeof(newSectorCount),
                        nullptr, 0, &bytesReturned, nullptr))
                {
                    FinalizeFail(L"FSCTL_EXTEND_VOLUME failed",
                        L"  diskpart:  select volume " + std::wstring(1, windowsLetter) + L"  /  extend filesystem");
                }

                fwprintf(stderr, L"Extended NTFS to fill the expanded partition.\n");
            }
        }
        catch (const Error& e)
        {
            windowsMount.mountPoint.clear();          // leave the letters mounted
            espMount.mountPoint.clear();
            throw Error(e.Code(), e.Message() +
                L"\n  (the temp drive letters above remain mounted so these commands work)");
        }

        wprintf(L"\nClone finalized with new identity. Both disks can remain connected.\n");
        wprintf(L"Note: if you boot the clone, WinRE may need re-registration there:\n");
        wprintf(L"      reagentc /disable && reagentc /enable\n");
    }

    void PrintSwapInstructions(const ClonePlan& plan, bool targetOffline)
    {
        wprintf(L"\nClone complete. The target disk keeps the source's identity.\n");
        if (targetOffline)
        {
            wprintf(L"It was left OFFLINE to avoid an identity collision while both disks are connected.\n");
        }
        else
        {
            wprintf(L"WARNING: this device does not support the offline attribute, so the clone is\n");
            wprintf(L"ONLINE with a duplicate disk identity. DISCONNECT IT NOW, before Windows\n");
            wprintf(L"resolves the collision by rewriting its identity (which would break booting).\n");
        }

        wprintf(L"\nTo use the clone:\n");
        wprintf(L"  1. Shut down and physically swap the drives (remove or replace the source).\n");
        wprintf(L"  2. Boot — firmware forces its boot disk online automatically.\n");
        if (plan.opts.expand)
        {
            wprintf(L"  3. The Windows partition was expanded but the filesystem is still the old\n");
            wprintf(L"     size. After first boot from the clone, run (elevated):\n");
            wprintf(L"       diskpart\n");
            wprintf(L"       select volume C\n");
            wprintf(L"       extend filesystem\n");
        }
    }
}
