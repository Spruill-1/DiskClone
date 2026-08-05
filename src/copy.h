#pragma once

#include "layout.h"

namespace dc {

// Copies one partition's data from a source device (shadow volume, live
// volume, or raw disk region) to targetDisk at the planned target offset.
//
// - NtfsBitmap strategy reads via a *volume* handle using
//   FSCTL_GET_VOLUME_BITMAP, copying only allocated clusters, plus the first
//   1 MiB of the volume and the NTFS backup boot sector. Shadow devices are
//   read as-is (immutable); live volumes are exclusively LOCKED for the copy
//   (which also flushes them) or the copy fails — a mounted writable volume
//   is never silently copied.
// - Raw strategy copies the whole partition span through the source *disk*
//   handle at the partition offset.
// The target disk handle is opened without NO_BUFFERING (buffered writes),
// and CmdClone issues FlushFileBuffers at the end of the copy loop.
class CopyEngine {
public:
    CopyEngine(HANDLE targetDisk, uint32_t sectorSize, Progress& progress)
        : target_(targetDisk), sectorSize_(sectorSize), progress_(progress) {}

    // sourcePath: volume-style path without trailing slash (shadow device or
    // \\?\Volume{...}) — opened here with NO_BUFFERING | SEQUENTIAL_SCAN.
    void CopyNtfsBitmap(const std::wstring& sourcePath, const PlannedPartition& part);

    // Raw copy via an already-open source disk handle.
    void CopyRawFromDisk(HANDLE sourceDisk, const PlannedPartition& part);

private:
    void WriteTarget(uint64_t targetOffset, const uint8_t* data, uint32_t bytes);
    void CopySpan(HANDLE source, uint64_t srcOffset, uint64_t tgtOffset, uint64_t bytes,
                  bool sourceIsVolumeRelative);

    HANDLE target_;
    uint32_t sectorSize_;
    Progress& progress_;
};

unique_handle OpenSourceVolumeForCopy(const std::wstring& pathNoSlash);

} // namespace dc
