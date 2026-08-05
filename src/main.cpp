#include "bootfix.h"
#include "copy.h"
#include "disks.h"
#include "layout.h"
#include "privileges.h"
#include "version.h"
#include "volumes.h"
#include "vss.h"

#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace dc {
volatile long g_cancelled = 0;
}

using namespace dc;

static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_cancelled, 1);
        fwprintf(stderr, L"\nCancelling... (finishing current block)\n");
        return TRUE;
    }
    return FALSE;
}

static void Usage() {
    wprintf(
        L"diskclone %s — clone physical disks on Windows, preserving bootability\n\n"
        L"Usage:\n"
        L"  diskclone list\n"
        L"  diskclone clone --source N --target N [--expand] [--new-ids] [--force] [--dry-run]\n"
        L"  diskclone --version\n\n",
        DC_VERSION_STRING);
    wprintf(
        L"Options:\n"
        L"  --source N   source physical disk number (see 'diskclone list')\n"
        L"  --target N   target physical disk number — ALL DATA ON IT IS DESTROYED\n"
        L"  --expand     grow the Windows partition to fill a larger target;\n"
        L"               trailing partitions (e.g. WinRE) are relocated to the disk end\n"
        L"  --new-ids    stamp fresh disk/partition IDs and repair boot files with bcdboot\n"
        L"               so source and clone can stay connected side by side\n"
        L"  --force      skip the interactive confirmation\n"
        L"  --dry-run    print the clone plan and exit without touching anything\n");
}

// ---------------------------------------------------------------------------
// list
// ---------------------------------------------------------------------------

static int CmdList() {
    auto disks = EnumerateDisks();
    auto vols = EnumerateVolumes();
    wprintf(L"%-5s %-26s %-8s %-10s %-6s %-9s %-7s %s\n",
        L"Disk", L"Model", L"Bus", L"Size", L"Style", L"Sector", L"State", L"Volumes");
    for (const auto& d : disks) {
        std::wstring letters;
        for (const auto& v : VolumesOnDisk(vols, d.number)) {
            if (v.letters.empty()) continue;
            if (!letters.empty()) letters += L" ";
            letters += v.letters;
        }
        wchar_t sector[16];
        swprintf_s(sector, L"%u/%u", d.logicalSectorSize, d.physicalSectorSize);
        std::wstring state = d.offline ? L"offline" : L"online";
        wprintf(L"%-5d %-26.26s %-8s %-10s %-6s %-9s %-7s %s%s\n",
            d.number, d.model.c_str(), d.busType.c_str(),
            FormatBytes(d.size).c_str(),
            d.style == PARTITION_STYLE_GPT ? L"GPT" : d.style == PARTITION_STYLE_MBR ? L"MBR" : L"RAW",
            sector, state.c_str(), letters.c_str(),
            d.isSystemDisk ? L"  [SYSTEM]" : L"");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// clone
// ---------------------------------------------------------------------------

// Strict decimal parse: the ENTIRE token must be digits (no trailing garbage,
// no hex, no sign) — "3x" or "0x2" must never quietly select a disk.
static bool ParseDiskNumber(const wchar_t* s, int& out) {
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s, &end, 10);
    if (end == s || v > 255) return false;
    while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') ++end;
    if (*end) return false;
    out = static_cast<int>(v);
    return true;
}

static void Confirm(const ClonePlan& plan) {
    if (plan.opts.force) return;
    wprintf(L"This will DESTROY ALL DATA on disk %d (%s, %s).\n",
        plan.opts.target, plan.targetDisk.model.c_str(),
        FormatBytes(plan.targetDisk.size).c_str());
    wprintf(L"Type the target disk number to continue: ");
    fflush(stdout);
    wchar_t line[32] = {};
    if (!fgetws(line, 32, stdin))
        throw Error(ExitCode::UserAbort, L"no confirmation input");
    const wchar_t* p = line;
    while (*p == L' ' || *p == L'\t') ++p;
    int typed = -1;
    if (!ParseDiskNumber(p, typed) || typed != plan.opts.target)
        throw Error(ExitCode::UserAbort, L"confirmation did not match the target disk number");
}

static int CmdClone(const CloneOptions& opts) {
    EnsureElevatedAndEnablePrivileges();

    auto disks = EnumerateDisks();
    auto vols = EnumerateVolumes();
    ClonePlan plan = BuildClonePlan(opts, disks, vols);
    PrintPlan(plan);
    if (opts.dryRun) return 0;
    Confirm(plan);

    // The confirmation prompt can sit for minutes and disk numbers get reused;
    // re-verify both disks' identity before anything irreversible.
    RevalidateDiskIdentity(plan.sourceDisk, L"source");
    RevalidateDiskIdentity(plan.targetDisk, L"target");

    // Open the source (read-only) BEFORE touching the target: a missing or
    // failing source must abort while "nothing was written" is still true.
    unique_handle sourceDisk = OpenPhysicalDisk(plan.sourceDisk.number, GENERIC_READ);

    // --- VSS snapshots, BEFORE target prep ------------------------------------
    // Every VSS precondition is checkable without touching the target; taking
    // the snapshot first means a VSS failure aborts with the target intact.
    // All source NTFS volumes are snapshotted — a mounted data disk is just as
    // torn by a live copy as the system disk would be.
    SnapshotSet snapshots;
    {
        std::vector<std::wstring> ntfsVolumes;
        for (const auto& p : plan.parts)
            if (p.strategy == CopyStrategy::NtfsBitmap && !p.srcVolumeGuidPath.empty())
                ntfsVolumes.push_back(p.srcVolumeGuidPath);
        if (!ntfsVolumes.empty())
            snapshots.Create(ntfsVolumes);
    }

    bool targetModified = false;   // set right before the first destructive IOCTL
    bool offlined = false;
    unique_handle target;
    std::vector<unique_handle> lockedVolumes;   // fallback path keeps these alive

    try {
        // --- Target prep -------------------------------------------------------
        target = OpenPhysicalDisk(plan.targetDisk.number, GENERIC_READ | GENERIC_WRITE);
        try {
            SetDiskOffline(target.get(), true);
            offlined = true;
            fwprintf(stderr, L"Target disk %d set offline.\n", plan.targetDisk.number);
        } catch (const Error&) {
            fwprintf(stderr, L"Target rejects offline (removable?); locking volumes instead.\n");
            for (const auto& v : VolumesOnDisk(EnumerateVolumes(), plan.targetDisk.number))
                lockedVolumes.push_back(LockAndDismountVolume(v.guidPath));
        }

        targetModified = true;
        DeleteDriveLayout(target.get());
        CreateDiskStyle(target.get(), plan.sourceDisk.style, plan.targetGptDiskId,
            plan.targetMbrSignature);
        SetDriveLayout(target.get(), BuildTargetLayoutBuffer(plan));
        UpdateDiskProperties(target.get());
        fwprintf(stderr, L"Partition table written to target.\n");

        if (plan.sourceDisk.style == PARTITION_STYLE_MBR)
            SpliceMbrBootstrap(sourceDisk.get(), target.get(), plan.sourceDisk.logicalSectorSize);

        if (!offlined) {
            // The target is still ONLINE: every partition volume that surfaces
            // must be locked before the copy writes filesystem data into it, or
            // mountmgr can mount a half-copied filesystem mid-run (lazy writes
            // would corrupt the clone). Poll until all volumes are locked;
            // an unlockable volume is a hard error, not a warning.
            std::vector<std::wstring> locked;
            for (int attempt = 0; attempt < 10; ++attempt) {
                Sleep(1000);
                bool allLocked = true;
                for (const auto& v : VolumesOnDisk(EnumerateVolumes(), plan.targetDisk.number)) {
                    if (std::find(locked.begin(), locked.end(), v.guidPath) != locked.end())
                        continue;
                    try {
                        lockedVolumes.push_back(LockAndDismountVolume(v.guidPath));
                        locked.push_back(v.guidPath);
                    } catch (const Error&) {
                        allLocked = false;   // retry next round
                    }
                }
                if (allLocked && attempt >= 2) break;   // two quiet rounds = stable
            }
            for (const auto& v : VolumesOnDisk(EnumerateVolumes(), plan.targetDisk.number))
                if (std::find(locked.begin(), locked.end(), v.guidPath) == locked.end())
                    throw Error(ExitCode::CopyFailure,
                        L"cannot lock volume " + v.guidPath +
                        L" on the online target; something is using it — aborting before the copy");
        }

        // --- Copy loop ----------------------------------------------------------
        Progress progress;
        progress.Begin(plan.totalCopyBytes);
        CopyEngine engine(target.get(), plan.sourceDisk.logicalSectorSize, progress);

        for (const auto& p : plan.parts) {
            CheckCancelled();
            if (p.strategy == CopyStrategy::NtfsBitmap && !p.srcVolumeGuidPath.empty()) {
                std::wstring src = snapshots.ShadowDevice(p.srcVolumeGuidPath);
                if (src.empty()) {
                    // VSS skipped this volume: the live volume will be locked
                    // for the copy or the copy fails (never a torn image).
                    src = p.srcVolumeGuidPath;
                    src.pop_back();   // strip trailing backslash
                }
                engine.CopyNtfsBitmap(src, p);
                continue;
            }
            engine.CopyRawFromDisk(sourceDisk.get(), p);
        }

        if (!FlushFileBuffers(target.get()))
            ThrowWin32(ExitCode::CopyFailure, L"FlushFileBuffers on target failed");
        progress.End();
        snapshots.Complete();   // best-effort; never throws

        // --- Finalize -----------------------------------------------------------
        // Locks must be gone before finalization mounts/writes the clone.
        lockedVolumes.clear();

        if (opts.newIds) {
            FinalizeNewIds(plan, target.get());
        } else {
            if (!offlined) {
                // Volumes are unlocked now; retry the offline attribute so the
                // duplicated identity is not left online next to the source.
                try { SetDiskOffline(target.get(), true); offlined = true; }
                catch (const Error&) { /* reported truthfully below */ }
            }
            PrintSwapInstructions(plan, offlined);
        }
        return 0;
    } catch (const Error& e) {
        // Annotate every post-confirmation failure with the target's true state.
        std::wstring note;
        if (targetModified) {
            note = L"\nTarget disk " + std::to_wstring(plan.targetDisk.number) +
                L": previous contents are DESTROYED and the clone is incomplete; "
                L"re-run diskclone or reinitialize it in Disk Management.";
            if (offlined)
                note += L"\nThe target remains OFFLINE (persistent attribute); "
                        L"online it in Disk Management to reuse it.";
        } else {
            if (offlined) {
                try { SetDiskOffline(target.get(), false); }
                catch (const Error&) {
                    note = L"\nNote: the target disk was left OFFLINE; online it in Disk Management.";
                }
            }
            note += L"\nTarget disk " + std::to_wstring(plan.targetDisk.number) +
                L" was NOT modified.";
        }
        throw Error(e.code(), e.message() + note);
    }
}

// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    try {
        if (argc < 2) { Usage(); return static_cast<int>(ExitCode::Usage); }
        std::wstring verb = argv[1];
        if (verb == L"--version" || verb == L"version") {
            wprintf(L"diskclone %s\n", DC_VERSION_STRING);
            return 0;
        }
        if (verb == L"list") return CmdList();
        if (verb == L"clone") {
            CloneOptions opts;
            for (int i = 2; i < argc; ++i) {
                std::wstring a = argv[i];
                if (a == L"--source" && i + 1 < argc) { if (!ParseDiskNumber(argv[++i], opts.source)) { Usage(); return 1; } }
                else if (a == L"--target" && i + 1 < argc) { if (!ParseDiskNumber(argv[++i], opts.target)) { Usage(); return 1; } }
                else if (a == L"--expand") opts.expand = true;
                else if (a == L"--new-ids") opts.newIds = true;
                else if (a == L"--force") opts.force = true;
                else if (a == L"--dry-run") opts.dryRun = true;
                else { fwprintf(stderr, L"unknown option: %s\n", a.c_str()); Usage(); return 1; }
            }
            if (opts.source < 0 || opts.target < 0) { Usage(); return static_cast<int>(ExitCode::Usage); }
            return CmdClone(opts);
        }
        Usage();
        return static_cast<int>(ExitCode::Usage);
    } catch (const Error& e) {
        fwprintf(stderr, L"\nerror: %s\n", e.message().c_str());
        return static_cast<int>(e.code());
    } catch (const std::exception& e) {
        fprintf(stderr, "\nfatal: %s\n", e.what());
        return static_cast<int>(ExitCode::CopyFailure);
    }
}
