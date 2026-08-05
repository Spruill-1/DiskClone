#pragma once

#include "util.h"
#include <map>

namespace dc {

// RAII VSS snapshot set over a group of NTFS volumes. On success, ShadowDevice()
// maps each original volume GUID path -> \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN.
// VSS_CTX_BACKUP snapshots are auto-release: they disappear when the backup
// components object is released (including on crash/unwind).
class SnapshotSet {
public:
    SnapshotSet() = default;
    ~SnapshotSet();
    SnapshotSet(const SnapshotSet&) = delete;
    SnapshotSet& operator=(const SnapshotSet&) = delete;

    // Attempts to add every volume; volumes that fail AddToSnapshotSet are
    // reported in Skipped() and the caller falls back to live raw copy.
    void Create(const std::vector<std::wstring>& volumeGuidPaths);

    // Empty string if the volume was skipped.
    std::wstring ShadowDevice(const std::wstring& volumeGuidPath) const;
    const std::vector<std::wstring>& Skipped() const { return skipped_; }

    // Call after a successful copy so writers record a successful backup.
    void Complete();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::map<std::wstring, std::wstring> shadowByVolume_;
    std::vector<std::wstring> skipped_;
};

} // namespace dc
