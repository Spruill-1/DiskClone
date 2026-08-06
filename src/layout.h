#pragma once

// The planning brain. BuildClonePlan runs the read-only safety gauntlet and
// produces an immutable ClonePlan describing exactly what the clone will do:
// per-partition source→target mapping, copy strategy, --expand relocation,
// and the identity (disk GUID / MBR signature, partition GUIDs) to stamp on
// the target. Everything destructive elsewhere consumes this plan verbatim —
// including which partition is "the Windows partition", identified exactly
// once here so --expand sizing and --new-ids finalization can never disagree.

#include "disks.h"
#include "volumes.h"

namespace DiskClone
{
    enum class CopyStrategy
    {
        NtfsBitmap,    // used-clusters only, via FSCTL_GET_VOLUME_BITMAP
        Raw,           // full partition span through the source disk handle
    };

    struct PlannedPartition
    {
        PartitionInfo src;                 // source partition (offset/length/ids)
        uint64_t targetOffset{ 0 };
        uint64_t targetLength{ 0 };        // > src.length only for the expanded Windows partition
        CopyStrategy strategy{ CopyStrategy::Raw };
        FsKind fs{ FsKind::Unknown };
        bool isWindowsPartition{ false };
        std::wstring srcVolumeGuidPath;    // empty if no volume (MSR)
    };

    struct CloneOptions
    {
        int source{ -1 };
        int target{ -1 };
        bool expand{ false };
        bool newIds{ false };
        bool force{ false };
        bool dryRun{ false };
    };

    struct ClonePlan
    {
        CloneOptions opts;
        DiskInfo sourceDisk;
        DiskInfo targetDisk;
        std::vector<PlannedPartition> parts;   // in on-disk (source) order
        uint64_t requiredTargetBytes{ 0 };
        uint64_t totalCopyBytes{ 0 };          // upper bound (bitmap parts count full length)

        // Identity to stamp on the target.
        GUID targetGptDiskId{};
        uint32_t targetMbrSignature{ 0 };
        bool sourceIsLive{ false };            // source hosts the running OS
    };

    // Runs the read-only safety gauntlet and builds the plan. Throws
    // DiskClone::Error with ExitCode::SafetyRefusal on any refusal.
    ClonePlan BuildClonePlan(const CloneOptions& opts,
                             const std::vector<DiskInfo>& disks,
                             const std::vector<VolumeInfo>& volumes);

    // Renders the plan as a human-readable table.
    void PrintPlan(const ClonePlan& plan);

    // Serializes the target DRIVE_LAYOUT_INFORMATION_EX for SET_DRIVE_LAYOUT_EX.
    std::vector<uint8_t> BuildTargetLayoutBuffer(const ClonePlan& plan);
}
