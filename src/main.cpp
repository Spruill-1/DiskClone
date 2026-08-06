// diskclone entry point: CLI parsing, the `list` verb, and the `clone`
// orchestration — the one place that sequences the destructive work:
//
//   plan (read-only) → confirm → revalidate identities → snapshot → wipe →
//   copy → finalize
//
// Ordering invariants worth knowing before touching this file:
//   - The source disk is opened and ALL VSS snapshots are taken BEFORE the
//     first destructive IOCTL, so every pre-wipe failure truthfully reports
//     "target not modified".
//   - Both disks' identities are re-verified after the confirmation prompt
//     (disk numbers get reused when devices come and go).
//   - Every post-confirmation failure appends the target's true state to the
//     error message; targetModified flips right before DeleteDriveLayout.

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

namespace DiskClone
{
    volatile long g_cancelled = 0;
}

using namespace DiskClone;

namespace
{
    BOOL WINAPI CtrlHandler(DWORD eventType)
    {
        if (eventType == CTRL_C_EVENT || eventType == CTRL_BREAK_EVENT || eventType == CTRL_CLOSE_EVENT)
        {
            InterlockedExchange(&g_cancelled, 1);
            fwprintf(stderr, L"\nCancelling... (finishing current block)\n");
            return TRUE;
        }

        return FALSE;
    }

    void Usage()
    {
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

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------

    int CmdList()
    {
        auto disks = EnumerateDisks();
        auto volumes = EnumerateVolumes();
        wprintf(L"%-5s %-26s %-8s %-10s %-6s %-9s %-7s %s\n",
            L"Disk", L"Model", L"Bus", L"Size", L"Style", L"Sector", L"State", L"Volumes");
        for (const auto& disk : disks)
        {
            std::wstring letters;
            for (const auto& volume : VolumesOnDisk(volumes, disk.number))
            {
                if (volume.letters.empty()) { continue; }
                if (!letters.empty()) { letters += L" "; }
                letters += volume.letters;
            }

            wchar_t sectorText[16];
            swprintf_s(sectorText, L"%u/%u", disk.logicalSectorSize, disk.physicalSectorSize);
            wprintf(L"%-5d %-26.26s %-8s %-10s %-6s %-9s %-7s %s%s\n",
                disk.number, disk.model.c_str(), disk.busType.c_str(),
                FormatBytes(disk.size).c_str(),
                disk.style == PARTITION_STYLE_GPT ? L"GPT" : disk.style == PARTITION_STYLE_MBR ? L"MBR" : L"RAW",
                sectorText, disk.offline ? L"offline" : L"online", letters.c_str(),
                disk.isSystemDisk ? L"  [SYSTEM]" : L"");
        }

        return 0;
    }

    // -----------------------------------------------------------------------
    // clone
    // -----------------------------------------------------------------------

    // Strict decimal parse: the ENTIRE token must be digits (no trailing
    // garbage, no hex, no sign) — "3x" or "0x2" must never quietly select a disk.
    bool ParseDiskNumber(const wchar_t* text, int& parsedNumber)
    {
        if (!text || !*text) { return false; }
        wchar_t* parseEnd = nullptr;
        unsigned long value = wcstoul(text, &parseEnd, 10);
        if (parseEnd == text || value > 255) { return false; }
        while (*parseEnd == L' ' || *parseEnd == L'\t' || *parseEnd == L'\r' || *parseEnd == L'\n') { ++parseEnd; }
        if (*parseEnd) { return false; }
        parsedNumber = static_cast<int>(value);
        return true;
    }

    void Confirm(const ClonePlan& plan)
    {
        if (plan.opts.force) { return; }
        wprintf(L"This will DESTROY ALL DATA on disk %d (%s, %s).\n",
            plan.opts.target, plan.targetDisk.model.c_str(),
            FormatBytes(plan.targetDisk.size).c_str());
        wprintf(L"Type the target disk number to continue: ");
        fflush(stdout);
        wchar_t line[32] = {};
        if (!fgetws(line, 32, stdin))
        {
            throw Error(ExitCode::UserAbort, L"no confirmation input");
        }

        const wchar_t* inputCursor = line;
        while (*inputCursor == L' ' || *inputCursor == L'\t') { ++inputCursor; }
        int typedNumber = -1;
        if (!ParseDiskNumber(inputCursor, typedNumber) || typedNumber != plan.opts.target)
        {
            throw Error(ExitCode::UserAbort, L"confirmation did not match the target disk number");
        }
    }

    int CmdClone(const CloneOptions& opts)
    {
        EnsureElevatedAndEnablePrivileges();

        auto disks = EnumerateDisks();
        auto volumes = EnumerateVolumes();
        ClonePlan plan = BuildClonePlan(opts, disks, volumes);
        PrintPlan(plan);
        if (opts.dryRun) { return 0; }
        Confirm(plan);

        // The confirmation prompt can sit for minutes and disk numbers get
        // reused; re-verify both disks' identity before anything irreversible.
        RevalidateDiskIdentity(plan.sourceDisk, L"source");
        RevalidateDiskIdentity(plan.targetDisk, L"target");

        // Open the source (read-only) BEFORE touching the target: a missing or
        // failing source must abort while "nothing was written" is still true.
        wil::unique_hfile sourceDisk = OpenPhysicalDisk(plan.sourceDisk.number, GENERIC_READ);

        // --- VSS snapshots, BEFORE target prep --------------------------------
        // Every VSS precondition is checkable without touching the target;
        // taking the snapshot first means a VSS failure aborts with the target
        // intact. All source NTFS volumes are snapshotted — a mounted data
        // disk is just as torn by a live copy as the system disk would be.
        SnapshotSet snapshots;
        {
            std::vector<std::wstring> ntfsVolumes;
            for (const auto& planned : plan.parts)
            {
                if (planned.strategy == CopyStrategy::NtfsBitmap && !planned.srcVolumeGuidPath.empty())
                {
                    ntfsVolumes.push_back(planned.srcVolumeGuidPath);
                }
            }

            if (!ntfsVolumes.empty())
            {
                snapshots.Create(ntfsVolumes);
            }
        }

        bool targetModified = false;   // set right before the first destructive IOCTL
        bool offlined = false;
        wil::unique_hfile target;
        std::vector<wil::unique_hfile> lockedVolumes;   // fallback path keeps these alive

        try
        {
            // --- Target prep --------------------------------------------------
            target = OpenPhysicalDisk(plan.targetDisk.number, GENERIC_READ | GENERIC_WRITE);
            try
            {
                SetDiskOffline(target.get(), true);
                offlined = true;
                fwprintf(stderr, L"Target disk %d set offline.\n", plan.targetDisk.number);
            }
            catch (const Error&)
            {
                fwprintf(stderr, L"Target rejects offline (removable?); locking volumes instead.\n");
                for (const auto& volume : VolumesOnDisk(EnumerateVolumes(), plan.targetDisk.number))
                {
                    lockedVolumes.push_back(LockAndDismountVolume(volume.guidPath));
                }
            }

            targetModified = true;
            DeleteDriveLayout(target.get());
            CreateDiskStyle(target.get(), plan.sourceDisk.style, plan.targetGptDiskId,
                plan.targetMbrSignature);
            SetDriveLayout(target.get(), BuildTargetLayoutBuffer(plan));
            UpdateDiskProperties(target.get());
            fwprintf(stderr, L"Partition table written to target.\n");

            if (plan.sourceDisk.style == PARTITION_STYLE_MBR)
            {
                SpliceMbrBootstrap(sourceDisk.get(), target.get(), plan.sourceDisk.logicalSectorSize);
            }

            if (!offlined)
            {
                // The target is still ONLINE: every partition volume that
                // surfaces must be locked before the copy writes filesystem
                // data into it, or mountmgr can mount a half-copied filesystem
                // mid-run (lazy writes would corrupt the clone). Poll until
                // all volumes are locked; an unlockable volume is a hard
                // error, not a warning.
                std::vector<std::wstring> lockedGuidPaths;
                for (int attempt = 0; attempt < 10; ++attempt)
                {
                    Sleep(1000);
                    bool allLocked = true;
                    for (const auto& volume : VolumesOnDisk(EnumerateVolumes(), plan.targetDisk.number))
                    {
                        if (std::find(lockedGuidPaths.begin(), lockedGuidPaths.end(), volume.guidPath) != lockedGuidPaths.end())
                        {
                            continue;
                        }

                        try
                        {
                            lockedVolumes.push_back(LockAndDismountVolume(volume.guidPath));
                            lockedGuidPaths.push_back(volume.guidPath);
                        }
                        catch (const Error&)
                        {
                            allLocked = false;   // retry next round
                        }
                    }

                    if (allLocked && attempt >= 2) { break; }   // two quiet rounds = stable
                }

                for (const auto& volume : VolumesOnDisk(EnumerateVolumes(), plan.targetDisk.number))
                {
                    if (std::find(lockedGuidPaths.begin(), lockedGuidPaths.end(), volume.guidPath) == lockedGuidPaths.end())
                    {
                        throw Error(ExitCode::CopyFailure,
                            L"cannot lock volume " + volume.guidPath +
                            L" on the online target; something is using it — aborting before the copy");
                    }
                }
            }

            // --- Copy loop ----------------------------------------------------
            Progress progress;
            progress.Begin(plan.totalCopyBytes);
            CopyEngine engine(target.get(), plan.sourceDisk.logicalSectorSize, progress);

            for (const auto& planned : plan.parts)
            {
                CheckCancelled();
                if (planned.strategy == CopyStrategy::NtfsBitmap && !planned.srcVolumeGuidPath.empty())
                {
                    std::wstring sourcePath = snapshots.ShadowDevice(planned.srcVolumeGuidPath);
                    if (sourcePath.empty())
                    {
                        // VSS skipped this volume: the live volume will be locked
                        // for the copy or the copy fails (never a torn image).
                        sourcePath = planned.srcVolumeGuidPath;
                        sourcePath.pop_back();   // strip trailing backslash
                    }

                    engine.CopyNtfsBitmap(sourcePath, planned);
                    continue;
                }

                engine.CopyRawFromDisk(sourceDisk.get(), planned);
            }

            if (!FlushFileBuffers(target.get()))
            {
                const DWORD lastError = GetLastError();
                ThrowWin32Error(ExitCode::CopyFailure, L"FlushFileBuffers on target failed", lastError);
            }

            progress.End();
            snapshots.Complete();   // best-effort; never throws

            // --- Finalize -----------------------------------------------------
            // Locks must be gone before finalization mounts/writes the clone.
            lockedVolumes.clear();

            if (opts.newIds)
            {
                FinalizeNewIds(plan, target.get());
            }
            else
            {
                if (!offlined)
                {
                    // Volumes are unlocked now; retry the offline attribute so
                    // the duplicated identity is not left online next to the
                    // source.
                    try
                    {
                        SetDiskOffline(target.get(), true);
                        offlined = true;
                    }
                    catch (const Error&)
                    {
                        // Reported truthfully by PrintSwapInstructions.
                    }
                }

                PrintSwapInstructions(plan, offlined);
            }

            return 0;
        }
        catch (const Error& e)
        {
            // Annotate every post-confirmation failure with the target's true state.
            std::wstring stateNote;
            if (targetModified)
            {
                stateNote = L"\nTarget disk " + std::to_wstring(plan.targetDisk.number) +
                    L": previous contents are DESTROYED and the clone is incomplete; "
                    L"re-run diskclone or reinitialize it in Disk Management.";
                if (offlined)
                {
                    stateNote += L"\nThe target remains OFFLINE (persistent attribute); "
                                 L"online it in Disk Management to reuse it.";
                }
            }
            else
            {
                if (offlined)
                {
                    try
                    {
                        SetDiskOffline(target.get(), false);
                    }
                    catch (const Error&)
                    {
                        stateNote = L"\nNote: the target disk was left OFFLINE; online it in Disk Management.";
                    }
                }

                stateNote += L"\nTarget disk " + std::to_wstring(plan.targetDisk.number) +
                    L" was NOT modified.";
            }

            throw Error(e.Code(), e.Message() + stateNote);
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    try
    {
        if (argc < 2)
        {
            Usage();
            return static_cast<int>(ExitCode::Usage);
        }

        std::wstring verb = argv[1];
        if (verb == L"--version" || verb == L"version")
        {
            wprintf(L"diskclone %s\n", DC_VERSION_STRING);
            return 0;
        }

        if (verb == L"list") { return CmdList(); }
        if (verb == L"clone")
        {
            CloneOptions opts;
            for (int i = 2; i < argc; ++i)
            {
                std::wstring arg = argv[i];
                if (arg == L"--source" && i + 1 < argc)
                {
                    if (!ParseDiskNumber(argv[++i], opts.source)) { Usage(); return 1; }
                }
                else if (arg == L"--target" && i + 1 < argc)
                {
                    if (!ParseDiskNumber(argv[++i], opts.target)) { Usage(); return 1; }
                }
                else if (arg == L"--expand") { opts.expand = true; }
                else if (arg == L"--new-ids") { opts.newIds = true; }
                else if (arg == L"--force") { opts.force = true; }
                else if (arg == L"--dry-run") { opts.dryRun = true; }
                else
                {
                    fwprintf(stderr, L"unknown option: %s\n", arg.c_str());
                    Usage();
                    return 1;
                }
            }

            if (opts.source < 0 || opts.target < 0)
            {
                Usage();
                return static_cast<int>(ExitCode::Usage);
            }

            return CmdClone(opts);
        }

        Usage();
        return static_cast<int>(ExitCode::Usage);
    }
    catch (const Error& e)
    {
        fwprintf(stderr, L"\nerror: %s\n", e.Message().c_str());
        return static_cast<int>(e.Code());
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "\nfatal: %s\n", e.what());
        return static_cast<int>(ExitCode::CopyFailure);
    }
}
