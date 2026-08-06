#pragma once

// The copy engine: moves one partition's data from a source device (shadow
// volume, live volume, or raw disk region) to the target disk at the planned
// target offset.
//
//   - NtfsBitmap strategy reads via a *volume* handle using
//     FSCTL_GET_VOLUME_BITMAP, copying only allocated clusters, plus the
//     first 1 MiB of the volume and the NTFS backup boot sector. Shadow
//     devices are read as-is (immutable); live volumes are exclusively
//     LOCKED for the copy (which also flushes them) or the copy fails — a
//     mounted writable volume is never silently copied.
//   - Raw strategy copies the whole partition span through the source *disk*
//     handle at the partition offset.
//
// The target disk handle is opened without NO_BUFFERING (buffered writes);
// the clone orchestration issues FlushFileBuffers at the end of the copy loop.

#include "layout.h"

namespace DiskClone
{
    class CopyEngine
    {
    public:
        CopyEngine(HANDLE targetDisk, uint32_t sectorSize, Progress& progress)
            : m_target(targetDisk), m_sectorSize(sectorSize), m_progress(progress)
        {
        }

        // sourcePath: volume-style path without trailing slash (shadow device
        // or \\?\Volume{...}) — opened here with NO_BUFFERING | SEQUENTIAL_SCAN.
        void CopyNtfsBitmap(const std::wstring& sourcePath, const PlannedPartition& partition);

        // Raw copy via an already-open source disk handle.
        void CopyRawFromDisk(HANDLE sourceDisk, const PlannedPartition& partition);

    private:
        void WriteTarget(uint64_t targetOffset, const uint8_t* data, uint32_t bytes);
        void CopySpan(HANDLE source, uint64_t sourceOffset, uint64_t targetOffset, uint64_t bytes);

        HANDLE m_target;
        uint32_t m_sectorSize;
        Progress& m_progress;
    };

    wil::unique_hfile OpenSourceVolumeForCopy(const std::wstring& pathNoSlash);
}
