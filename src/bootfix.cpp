#include "bootfix.h"
#include "volumes.h"

#include <winioctl.h>

#include <algorithm>

namespace dc {

void SpliceMbrBootstrap(HANDLE sourceDisk, HANDLE targetDisk, uint32_t sectorSize) {
    AlignedBuffer src(std::max<uint32_t>(sectorSize, 4096));
    AlignedBuffer tgt(std::max<uint32_t>(sectorSize, 4096));
    OVERLAPPED ov{};
    DWORD n = 0;
    if (!ReadFile(sourceDisk, src.data(), sectorSize, &n, &ov) || n != sectorSize)
        ThrowWin32(ExitCode::CopyFailure, L"failed to read source MBR");
    OVERLAPPED ov2{};
    if (!ReadFile(targetDisk, tgt.data(), sectorSize, &n, &ov2) || n != sectorSize)
        ThrowWin32(ExitCode::CopyFailure, L"failed to read target MBR");
    memcpy(tgt.data(), src.data(), 440);        // bootstrap only; keep sig+table+55AA
    OVERLAPPED ov3{};
    if (!WriteFile(targetDisk, tgt.data(), sectorSize, &n, &ov3) || n != sectorSize)
        ThrowWin32(ExitCode::CopyFailure, L"failed to write target MBR");
}

// ---------------------------------------------------------------------------
// --new-ids finalization
// ---------------------------------------------------------------------------

namespace {

[[noreturn]] void FinalizeFail(const std::wstring& why, const std::wstring& recovery) {
    throw Error(ExitCode::FinalizeFailure,
        why + L"\nThe data copy itself succeeded. Finish manually:\n" + recovery);
}

// Waits for the volume backing (diskNumber, partitionOffset) to appear.
std::wstring WaitForVolume(int diskNumber, uint64_t offset, DWORD timeoutMs) {
    ULONGLONG start = GetTickCount64();
    for (;;) {
        for (const auto& v : EnumerateVolumes())
            if (v.diskNumber == diskNumber && v.offset == offset)
                return v.guidPath;
        if (GetTickCount64() - start > timeoutMs) return L"";
        Sleep(500);
    }
}

struct TempMount {
    std::wstring mountPoint;   // "W:\\"
    ~TempMount() {
        if (!mountPoint.empty()) DeleteVolumeMountPointW(mountPoint.c_str());
    }
};

wchar_t FindFreeDriveLetter() {
    DWORD mask = GetLogicalDrives();
    for (wchar_t l = L'Z'; l >= L'G'; --l)
        if (!(mask & (1u << (l - L'A')))) return l;
    return 0;
}

bool MountAt(const std::wstring& volumeGuidPath, wchar_t letter, TempMount& tm) {
    std::wstring mp = std::wstring(1, letter) + L":\\";
    if (!SetVolumeMountPointW(mp.c_str(), volumeGuidPath.c_str())) return false;
    tm.mountPoint = mp;
    return true;
}

int RunProcess(const std::wstring& cmdLine) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdLine;   // CreateProcessW needs a writable buffer
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi))
        return -1;
    unique_handle hp(pi.hProcess), ht(pi.hThread);
    WaitForSingleObject(hp.get(), INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(hp.get(), &code);
    return static_cast<int>(code);
}

std::wstring System32Path(const wchar_t* exe) {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    return std::wstring(sysdir) + L"\\" + exe;
}

struct RegKeyGuard {
    HKEY key = nullptr;
    ~RegKeyGuard() { if (key) RegCloseKey(key); }
};

// With fresh partition GUIDs (and a fresh MBR signature), the clone's
// HKLM\SYSTEM\MountedDevices still maps \DosDevices\C: to the OLD identity;
// at first boot mountmgr would assign the boot volume a different letter and
// the system breaks. Standard fix: clear MountedDevices in the clone's own
// SYSTEM hive so mountmgr regenerates it at boot. Requires SeBackup/SeRestore
// (enabled at startup) for RegLoadKey.
void ResetCloneMountedDevices(wchar_t winLetter) {
    const wchar_t* loadName = L"diskclone-clone-SYSTEM";
    std::wstring hivePath = std::wstring(1, winLetter) + L":\\Windows\\System32\\config\\SYSTEM";
    LSTATUS st = RegLoadKeyW(HKEY_LOCAL_MACHINE, loadName, hivePath.c_str());
    if (st != ERROR_SUCCESS)
        FinalizeFail(L"cannot load the clone's SYSTEM hive to reset MountedDevices (error " +
                std::to_wstring(st) + L")",
            L"  On the clone's first boot, if drive letters are wrong: boot WinRE,\n"
            L"  load the clone's SYSTEM hive in regedit and delete the values under\n"
            L"  MountedDevices, then reboot.");
    struct HiveUnloader {
        const wchar_t* name;
        ~HiveUnloader() { RegUnLoadKeyW(HKEY_LOCAL_MACHINE, name); }
    } unloader{ loadName };

    RegKeyGuard md;
    std::wstring subkey = std::wstring(loadName) + L"\\MountedDevices";
    st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, &md.key);
    if (st == ERROR_FILE_NOT_FOUND) return;   // nothing recorded; mountmgr will populate
    if (st != ERROR_SUCCESS)
        FinalizeFail(L"cannot open the clone's MountedDevices key (error " + std::to_wstring(st) + L")",
            L"  Delete the values under MountedDevices in the clone's SYSTEM hive manually.");
    for (;;) {
        wchar_t name[512];
        DWORD nameLen = ARRAYSIZE(name);
        st = RegEnumValueW(md.key, 0, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (st != ERROR_SUCCESS) break;
        if (RegDeleteValueW(md.key, name) != ERROR_SUCCESS) break;
    }
}

} // namespace

void FinalizeNewIds(const ClonePlan& plan, HANDLE targetDisk) {
    const bool gpt = plan.sourceDisk.style == PARTITION_STYLE_GPT;

    // The Windows partition was identified ONCE during planning (layout.cpp
    // refuses --new-ids when it cannot be identified); no second heuristic here.
    const PlannedPartition* winPart = nullptr;
    const PlannedPartition* bootPart = nullptr;
    for (const auto& p : plan.parts) {
        if (p.isWindowsPartition) winPart = &p;
        if (gpt && IsEqualGUID(p.src.gptType, kEspType)) bootPart = &p;
        if (!gpt && p.src.mbrActive) bootPart = &p;
    }
    if (!winPart)
        FinalizeFail(L"internal error: no Windows partition was flagged in the plan",
            L"  1. Online the disk in Disk Management\n"
            L"  2. bcdboot <clone>:\\Windows /s <esp>: /f " + std::wstring(gpt ? L"UEFI" : L"BIOS"));
    // BIOS boot files may live on the Windows partition itself when no
    // partition is marked active; GPT strictly requires the ESP.
    if (gpt && !bootPart)
        FinalizeFail(L"the clone has no EFI System Partition; cannot repair boot files",
            L"  bcdboot <clone>:\\Windows /s <esp>: /f UEFI");

    // 1. Bring the target online.
    try {
        SetDiskOffline(targetDisk, false);
    } catch (const Error& e) {
        // Data copy succeeded; onlining is the only thing that failed.
        FinalizeFail(L"could not bring the target disk online (" + e.message() + L")",
            L"  1. Disk Management: right-click disk " + std::to_wstring(plan.targetDisk.number) +
            L" -> Online\n"
            L"  2. bcdboot <clone>:\\Windows /s <esp>: /f " + std::wstring(gpt ? L"UEFI" : L"BIOS"));
    }
    UpdateDiskProperties(targetDisk);

    // 2. Wait for volumes to arrive.
    std::wstring winVol = WaitForVolume(plan.targetDisk.number, winPart->targetOffset, 15000);
    if (winVol.empty())
        FinalizeFail(L"the clone's Windows volume did not come online",
            L"  bcdboot <clone>:\\Windows /s <esp>: /f " + std::wstring(gpt ? L"UEFI" : L"BIOS"));

    // 3. Temp mounts.
    TempMount winMount, bootMount;
    wchar_t wl = FindFreeDriveLetter();
    if (!wl || !MountAt(winVol, wl, winMount))
        FinalizeFail(L"cannot assign a temp drive letter to the clone's Windows volume",
            L"  bcdboot <clone>:\\Windows /s <esp>: /f " + std::wstring(gpt ? L"UEFI" : L"BIOS"));

    // bcdboot MUST always receive /s: without it, it writes to the RUNNING
    // system's own boot partition — corrupting this machine's boot config to
    // point at a temporary drive letter. If the clone's boot partition cannot
    // be mounted, fail instead of falling through.
    std::wstring bootSwitch;
    TempMount espMount;
    const PlannedPartition* bootTarget = bootPart ? bootPart : winPart;   // BIOS: boot files can live on the Windows partition
    {
        std::wstring bootVol = WaitForVolume(plan.targetDisk.number, bootTarget->targetOffset, 15000);
        if (bootTarget == winPart) {
            bootSwitch = L" /s " + std::wstring(1, wl) + L":";
        } else if (bootVol.empty()) {
            FinalizeFail(L"the clone's boot partition (ESP/system) did not come online",
                L"  bcdboot " + std::wstring(1, wl) + L":\\Windows /s <esp>: /f " +
                std::wstring(gpt ? L"UEFI" : L"BIOS"));
        } else {
            wchar_t bl = 0;
            DWORD mask = GetLogicalDrives();
            for (wchar_t l = L'Z'; l >= L'G'; --l)
                if (l != wl && !(mask & (1u << (l - L'A')))) { bl = l; break; }
            if (!bl || !MountAt(bootVol, bl, espMount))
                FinalizeFail(L"cannot assign a temp drive letter to the clone's boot partition",
                    L"  bcdboot " + std::wstring(1, wl) + L":\\Windows /s <esp>: /f " +
                    std::wstring(gpt ? L"UEFI" : L"BIOS"));
            bootSwitch = L" /s " + std::wstring(1, bl) + L":";
        }
    }

    // Steps 4-6: on any failure below, KEEP the temp drive letters mounted so
    // the printed recovery commands (which reference them) remain valid.
    try {
        // 4. bcdboot — bootSwitch is guaranteed non-empty by construction above.
        std::wstring cmd = System32Path(L"bcdboot.exe") + L" " + std::wstring(1, wl) + L":\\Windows" +
            bootSwitch + L" /f " + (gpt ? L"UEFI" : L"BIOS");
        fwprintf(stderr, L"Running: %s\n", cmd.c_str());
        int rc = RunProcess(cmd);
        if (rc != 0)
            FinalizeFail(L"bcdboot failed with exit code " + std::to_wstring(rc),
                L"  " + cmd);

        // 5. Reset the clone's MountedDevices so its fresh partition IDs get
        //    re-mapped at first boot (drive-letter preservation).
        ResetCloneMountedDevices(wl);

        // 6. Extend the NTFS filesystem if --expand grew the Windows partition.
        if (plan.opts.expand && winPart->targetLength > winPart->src.length) {
            std::wstring volNoSlash = winVol;
            if (!volNoSlash.empty() && volNoSlash.back() == L'\\') volNoSlash.pop_back();
            HANDLE hv = CreateFileW(volNoSlash.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hv == INVALID_HANDLE_VALUE)
                FinalizeFail(L"cannot open the clone's Windows volume to extend it",
                    L"  diskpart:  select volume " + std::wstring(1, wl) + L"  /  extend filesystem");
            unique_handle uhv(hv);
            LONGLONG sectors = static_cast<LONGLONG>(winPart->targetLength / plan.sourceDisk.logicalSectorSize);
            DWORD ret = 0;
            if (!DeviceIoControl(uhv.get(), FSCTL_EXTEND_VOLUME, &sectors, sizeof(sectors),
                                 nullptr, 0, &ret, nullptr))
                FinalizeFail(L"FSCTL_EXTEND_VOLUME failed",
                    L"  diskpart:  select volume " + std::wstring(1, wl) + L"  /  extend filesystem");
            fwprintf(stderr, L"Extended NTFS to fill the expanded partition.\n");
        }
    } catch (const Error& e) {
        winMount.mountPoint.clear();          // leave the letters mounted
        espMount.mountPoint.clear();
        throw Error(e.code(), e.message() +
            L"\n  (the temp drive letters above remain mounted so these commands work)");
    }

    wprintf(L"\nClone finalized with new identity. Both disks can remain connected.\n");
    wprintf(L"Note: if you boot the clone, WinRE may need re-registration there:\n");
    wprintf(L"      reagentc /disable && reagentc /enable\n");
}

void PrintSwapInstructions(const ClonePlan& plan, bool targetOffline) {
    wprintf(L"\nClone complete. The target disk keeps the source's identity.\n");
    if (targetOffline) {
        wprintf(L"It was left OFFLINE to avoid an identity collision while both disks are connected.\n");
    } else {
        wprintf(L"WARNING: this device does not support the offline attribute, so the clone is\n");
        wprintf(L"ONLINE with a duplicate disk identity. DISCONNECT IT NOW, before Windows\n");
        wprintf(L"resolves the collision by rewriting its identity (which would break booting).\n");
    }
    wprintf(L"\nTo use the clone:\n");
    wprintf(L"  1. Shut down and physically swap the drives (remove or replace the source).\n");
    wprintf(L"  2. Boot — firmware forces its boot disk online automatically.\n");
    if (plan.opts.expand) {
        wprintf(L"  3. The Windows partition was expanded but the filesystem is still the old\n");
        wprintf(L"     size. After first boot from the clone, run (elevated):\n");
        wprintf(L"       diskpart\n");
        wprintf(L"       select volume C\n");
        wprintf(L"       extend filesystem\n");
    }
}

} // namespace dc
