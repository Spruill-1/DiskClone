#pragma once

// Volume enumeration and the volume↔disk mapping the safety gauntlet depends
// on. Used by:
//   - The planner, to find which volumes back which partitions (VSS targets,
//     BitLocker sniffing, system/pagefile/boot-disk annotation).
//   - Target prep, for the lock/dismount fallback when a disk rejects the
//     offline attribute.
//   - --new-ids finalization, to wait for the clone's volumes to arrive.

#include "util.h"
#include <optional>

namespace DiskClone
{
    struct DiskInfo; // disks.h

    struct VolumeInfo
    {
        std::wstring guidPath;                 // \\?\Volume{...}\  (trailing backslash)
        std::wstring letters;                  // display string: "C:", or folder mounts
        std::vector<std::wstring> mountPaths;  // raw mount paths incl. trailing backslash
        int diskNumber{ -1 };                  // -1 if multi-disk or unknown
        uint64_t offset{ 0 };                  // extent start on that disk
        uint64_t length{ 0 };
        bool multiExtent{ false };
    };

    // All volumes on the machine with their disk extents resolved.
    std::vector<VolumeInfo> EnumerateVolumes();

    // Volumes whose extents live on the given disk.
    std::vector<VolumeInfo> VolumesOnDisk(const std::vector<VolumeInfo>& all, int diskNumber);

    // Sets DiskInfo::isSystemDisk / isBootDisk / hasPagefile across the fleet.
    void AnnotateSystemAndPagefileDisks(std::vector<DiskInfo>& disks);

    // Filesystem sniffing: reads the first sector of a partition via the DISK
    // handle (works while volumes are mounted, no volume handle needed).
    enum class FsKind
    {
        Unknown,
        Ntfs,
        Fat,
        BitLocker,     // -FVE-FS- signature: encrypted OR suspended — refused either way
        Refs,
    };

    FsKind SniffFilesystem(HANDLE disk, uint64_t partitionOffset, uint32_t sectorSize);
    const wchar_t* FsKindName(FsKind kind);

    // Lock + dismount a volume and keep the handle (fallback target-prep path).
    wil::unique_hfile LockAndDismountVolume(const std::wstring& guidPath);

    // Volume GUID path of the volume backing the running OS (C:\Windows).
    std::wstring SystemVolumeGuidPath();
}
