#pragma once

// RAII VSS snapshot set over a group of NTFS volumes, via the full requester
// API (IVssBackupComponents). On success, ShadowDevice() maps each original
// volume GUID path to its point-in-time block device
// (\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN), which the copy engine
// reads instead of the live volume.
//
// VSS_CTX_BACKUP snapshots are auto-release: they disappear when the backup
// components object is released, including on crash/unwind — a failed clone
// never leaves stray shadow copies behind.

#include "util.h"
#include <map>

namespace DiskClone
{
    class SnapshotSet
    {
    public:
        // Both defined out-of-line where Impl is complete — required for the
        // std::unique_ptr pimpl (member cleanup instantiates Impl's deleter).
        SnapshotSet();
        ~SnapshotSet();

        SnapshotSet(const SnapshotSet&) = delete;
        SnapshotSet& operator=(const SnapshotSet&) = delete;

        // Attempts to add every volume; volumes that fail AddToSnapshotSet are
        // reported in Skipped() and the caller falls back to the lock-or-fail
        // live-copy path (never a silent torn copy).
        void Create(const std::vector<std::wstring>& volumeGuidPaths);

        // Empty string if the volume was skipped.
        std::wstring ShadowDevice(const std::wstring& volumeGuidPath) const;
        const std::vector<std::wstring>& Skipped() const { return m_skipped; }

        // Call after a successful copy so writers record a successful backup.
        // Best-effort: by then the copy has been flushed, and a writer hiccup
        // must not convert a valid clone into a reported failure.
        void Complete();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        std::map<std::wstring, std::wstring> m_shadowByVolume;
        std::vector<std::wstring> m_skipped;
    };
}
