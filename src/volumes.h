#pragma once

#include "util.h"
#include <optional>

namespace dc {

struct DiskInfo; // disks.h

struct VolumeInfo {
    std::wstring guidPath;             // \\?\Volume{...}\  (trailing backslash)
    std::wstring letters;              // display string: "C:", or folder mounts
    std::vector<std::wstring> mountPaths;  // raw mount paths incl. trailing backslash
    int diskNumber = -1;               // -1 if multi-disk or unknown
    uint64_t offset = 0;               // extent start on that disk
    uint64_t length = 0;
    bool multiExtent = false;
};

// All volumes on the machine with their disk extents resolved.
std::vector<VolumeInfo> EnumerateVolumes();

// Volumes whose extents live on the given disk.
std::vector<VolumeInfo> VolumesOnDisk(const std::vector<VolumeInfo>& all, int diskNumber);

// Sets DiskInfo::isSystemDisk / hasPagefile across the fleet.
void AnnotateSystemAndPagefileDisks(std::vector<DiskInfo>& disks);

// Filesystem sniffing: reads the first sector of a partition via the DISK
// handle (works while volumes are mounted, no volume handle needed).
enum class FsKind { Unknown, Ntfs, Fat, BitLocker, Refs };
FsKind SniffFilesystem(HANDLE disk, uint64_t partitionOffset, uint32_t sectorSize);
const wchar_t* FsKindName(FsKind k);

// Lock + dismount a volume and keep the handle (fallback target-prep path).
unique_handle LockAndDismountVolume(const std::wstring& guidPath);

// Volume GUID path of the volume backing the running OS (C:\Windows).
std::wstring SystemVolumeGuidPath();

} // namespace dc
