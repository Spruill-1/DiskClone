#include "copy.h"

#include <winioctl.h>

#include <algorithm>

namespace dc {

namespace {

constexpr uint32_t kChunk = 8 * 1024 * 1024;   // 8 MiB aligned I/O buffer

struct Run { uint64_t offset; uint64_t length; }; // byte offsets, volume-relative

void ReadAt(HANDLE h, uint64_t offset, uint8_t* buf, uint32_t bytes, const wchar_t* what) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    if (!ReadFile(h, buf, bytes, &read, &ov) || read != bytes)
        ThrowWin32(ExitCode::CopyFailure,
            std::wstring(what) + L" read failed at offset " + std::to_wstring(offset));
}

} // namespace

unique_handle OpenSourceVolumeForCopy(const std::wstring& pathNoSlash) {
    HANDLE h = CreateFileW(pathNoSlash.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ThrowWin32(ExitCode::CopyFailure, L"cannot open source volume " + pathNoSlash);
    unique_handle vol(h);

    // Shadow devices are immutable point-in-time images; live volumes are NOT.
    // Reading a mounted, writable volume raw yields a torn image (bitmap, MFT
    // and data observed at different instants). Take an exclusive lock — which
    // also flushes the volume — or refuse; never silently copy a moving target.
    bool isShadow = pathNoSlash.rfind(L"\\\\?\\GLOBALROOT", 0) == 0;
    if (!isShadow) {
        DWORD ret = 0;
        if (!DeviceIoControl(vol.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ret, nullptr))
            ThrowWin32(ExitCode::CopyFailure,
                L"cannot lock source volume " + pathNoSlash +
                L" for a consistent copy (it is in use and no VSS snapshot is available); "
                L"close programs using it and retry");
        // Lock is held for the life of this handle (the whole partition copy).
    }
    return vol;
}

void CopyEngine::WriteTarget(uint64_t targetOffset, const uint8_t* data, uint32_t bytes) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(targetOffset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(targetOffset >> 32);
    DWORD written = 0;
    if (!WriteFile(target_, data, bytes, &written, &ov) || written != bytes)
        ThrowWin32(ExitCode::CopyFailure,
            L"target write failed at offset " + std::to_wstring(targetOffset));
}

void CopyEngine::CopySpan(HANDLE source, uint64_t srcOffset, uint64_t tgtOffset,
                          uint64_t bytes, bool /*sourceIsVolumeRelative*/) {
    // Every span we produce (partition lengths, cluster runs, the 1 MiB head,
    // single sectors) is a sector multiple; a misreported sector size from a
    // flaky USB bridge could break that, so verify instead of assuming.
    if (srcOffset % sectorSize_ || tgtOffset % sectorSize_ || bytes % sectorSize_)
        throw Error(ExitCode::CopyFailure,
            L"internal: copy span not sector-aligned (offsets " + std::to_wstring(srcOffset) +
            L"/" + std::to_wstring(tgtOffset) + L", length " + std::to_wstring(bytes) +
            L", sector " + std::to_wstring(sectorSize_) + L")");
    AlignedBuffer buf(kChunk);
    uint64_t done = 0;
    while (done < bytes) {
        CheckCancelled();
        uint64_t remain = bytes - done;
        uint32_t ioBytes = static_cast<uint32_t>(std::min<uint64_t>(remain, kChunk));
        ReadAt(source, srcOffset + done, buf.data(), ioBytes, L"source");
        WriteTarget(tgtOffset + done, buf.data(), ioBytes);
        done += ioBytes;
        progress_.Add(ioBytes);
    }
}

void CopyEngine::CopyRawFromDisk(HANDLE sourceDisk, const PlannedPartition& part) {
    CopySpan(sourceDisk, part.src.offset, part.targetOffset, part.src.length, false);
}

void CopyEngine::CopyNtfsBitmap(const std::wstring& sourcePath, const PlannedPartition& part) {
    unique_handle vol = OpenSourceVolumeForCopy(sourcePath);

    NTFS_VOLUME_DATA_BUFFER nvd{};
    DWORD ret = 0;
    if (!DeviceIoControl(vol.get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                         &nvd, sizeof(nvd), &ret, nullptr))
        ThrowWin32(ExitCode::CopyFailure, L"FSCTL_GET_NTFS_VOLUME_DATA failed for " + sourcePath);
    const uint64_t clusterSize = static_cast<uint64_t>(nvd.BytesPerCluster);

    // Gather allocated runs from the volume bitmap, coalescing adjacent and
    // near-adjacent (< 1 MiB gap) runs into large sequential extents.
    std::vector<Run> runs;
    {
        const uint32_t bitmapChunk = 4 * 1024 * 1024;   // bits for ~32M clusters per call
        std::vector<uint8_t> out(sizeof(VOLUME_BITMAP_BUFFER) + bitmapChunk);
        uint64_t lcn = 0;
        bool more = true;
        uint64_t runStart = UINT64_MAX, runLen = 0;
        auto flushRun = [&]() {
            if (runStart == UINT64_MAX) return;
            uint64_t byteOff = runStart * clusterSize;
            uint64_t byteLen = runLen * clusterSize;
            if (!runs.empty()) {
                Run& last = runs.back();
                if (byteOff - (last.offset + last.length) < kMiB) {
                    last.length = byteOff + byteLen - last.offset;   // bridge small gap
                    runStart = UINT64_MAX; runLen = 0;
                    return;
                }
            }
            runs.push_back({ byteOff, byteLen });
            runStart = UINT64_MAX; runLen = 0;
        };
        while (more) {
            CheckCancelled();
            STARTING_LCN_INPUT_BUFFER in{};
            in.StartingLcn.QuadPart = static_cast<LONGLONG>(lcn);
            DWORD bytes = 0;
            BOOL ok = DeviceIoControl(vol.get(), FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in),
                out.data(), static_cast<DWORD>(out.size()), &bytes, nullptr);
            DWORD err = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && err != ERROR_MORE_DATA)
                ThrowWin32Err(ExitCode::CopyFailure, L"FSCTL_GET_VOLUME_BITMAP failed", err);
            more = !ok;   // ERROR_MORE_DATA -> more chunks follow
            auto* vb = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(out.data());
            uint64_t base = static_cast<uint64_t>(vb->StartingLcn.QuadPart);
            uint64_t count = static_cast<uint64_t>(vb->BitmapSize.QuadPart);
            uint64_t usable = std::min<uint64_t>(count, (bytes - offsetof(VOLUME_BITMAP_BUFFER, Buffer)) * 8ull);
            for (uint64_t i = 0; i < usable; ++i) {
                bool allocated = (vb->Buffer[i >> 3] >> (i & 7)) & 1;
                uint64_t cluster = base + i;
                if (allocated) {
                    if (runStart == UINT64_MAX) { runStart = cluster; runLen = 1; }
                    else if (cluster == runStart + runLen) { ++runLen; }
                    else { flushRun(); runStart = cluster; runLen = 1; }
                } else if (runStart != UINT64_MAX) {
                    flushRun();
                }
            }
            lcn = base + usable;
            if (usable == 0) break;
        }
        flushRun();
    }

    // Region 1: first 1 MiB of the volume ($Boot and friends), unconditionally.
    {
        uint64_t head = std::min<uint64_t>(kMiB, part.src.length);
        CopySpan(vol.get(), 0, part.targetOffset, head, true);
    }

    // Allocated cluster runs (skip what the head copy already covered).
    for (const Run& r : runs) {
        uint64_t off = r.offset, len = r.length;
        if (off + len <= kMiB) continue;
        if (off < kMiB) { len -= kMiB - off; off = kMiB; }
        CopySpan(vol.get(), off, part.targetOffset + off, len, true);
    }

    // Region 2: NTFS backup boot sector — the sector at NumberSectors (the BPB
    // total excludes it), past the last cluster and absent from the bitmap.
    // A mounted volume handle cannot read past NumberSectors (EOF), but the
    // backup is by definition a copy of the primary boot sector, so replicate
    // sector 0 into the backup position. Offsets are source-volume-relative,
    // so this lands correctly even when the target partition is larger
    // (--expand): the FS is still source-sized.
    {
        uint64_t backupOff = static_cast<uint64_t>(nvd.NumberSectors.QuadPart) * sectorSize_;
        if (backupOff + sectorSize_ <= part.src.length) {
            AlignedBuffer sec(sectorSize_);
            ReadAt(vol.get(), 0, sec.data(), sectorSize_, L"boot sector");
            WriteTarget(part.targetOffset + backupOff, sec.data(), sectorSize_);
        }
    }
}

} // namespace dc
