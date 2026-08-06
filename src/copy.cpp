#include "copy.h"

#include <winioctl.h>

#include <algorithm>

namespace DiskClone
{
    namespace
    {
        constexpr uint32_t kChunk = 8 * 1024 * 1024;   // 8 MiB aligned I/O buffer

        struct Run
        {
            uint64_t offset;   // byte offset, volume-relative
            uint64_t length;   // bytes
        };

        void ReadAt(HANDLE handle, uint64_t offset, uint8_t* buffer, uint32_t bytes, const wchar_t* what)
        {
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
            overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD bytesRead = 0;
            if (!ReadFile(handle, buffer, bytes, &bytesRead, &overlapped) || bytesRead != bytes)
            {
                const DWORD lastError = GetLastError();
                ThrowWin32Error(ExitCode::CopyFailure,
                    std::wstring(what) + L" read failed at offset " + std::to_wstring(offset), lastError);
            }
        }
    }

    wil::unique_hfile OpenSourceVolumeForCopy(const std::wstring& pathNoSlash)
    {
        wil::unique_hfile volume{ CreateFileW(pathNoSlash.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
        if (!volume)
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"cannot open source volume " + pathNoSlash, lastError);
        }

        // Shadow devices are immutable point-in-time images; live volumes are
        // NOT. Reading a mounted, writable volume raw yields a torn image
        // (bitmap, MFT and data observed at different instants). Take an
        // exclusive lock — which also flushes the volume — or refuse; never
        // silently copy a moving target.
        bool isShadowDevice = pathNoSlash.rfind(L"\\\\?\\GLOBALROOT", 0) == 0;
        if (!isShadowDevice)
        {
            DWORD bytesReturned = 0;
            if (!DeviceIoControl(volume.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr))
            {
                const DWORD lastError = GetLastError();
                ThrowWin32Error(ExitCode::CopyFailure,
                    L"cannot lock source volume " + pathNoSlash +
                    L" for a consistent copy (it is in use and no VSS snapshot is available); "
                    L"close programs using it and retry", lastError);
            }

            // The lock is held for the life of this handle (the whole partition copy).
        }

        return volume;
    }

    void CopyEngine::WriteTarget(uint64_t targetOffset, const uint8_t* data, uint32_t bytes)
    {
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(targetOffset & 0xFFFFFFFF);
        overlapped.OffsetHigh = static_cast<DWORD>(targetOffset >> 32);
        DWORD bytesWritten = 0;
        if (!WriteFile(m_target, data, bytes, &bytesWritten, &overlapped) || bytesWritten != bytes)
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure,
                L"target write failed at offset " + std::to_wstring(targetOffset), lastError);
        }
    }

    void CopyEngine::CopySpan(HANDLE source, uint64_t sourceOffset, uint64_t targetOffset, uint64_t bytes)
    {
        // Every span we produce (partition lengths, cluster runs, the 1 MiB
        // head, single sectors) is a sector multiple; a misreported sector
        // size from a flaky USB bridge could break that, so verify instead of
        // assuming.
        if (sourceOffset % m_sectorSize || targetOffset % m_sectorSize || bytes % m_sectorSize)
        {
            throw Error(ExitCode::CopyFailure,
                L"internal: copy span not sector-aligned (offsets " + std::to_wstring(sourceOffset) +
                L"/" + std::to_wstring(targetOffset) + L", length " + std::to_wstring(bytes) +
                L", sector " + std::to_wstring(m_sectorSize) + L")");
        }

        auto buffer = AllocateAlignedBuffer(kChunk);
        uint64_t bytesCopied = 0;
        while (bytesCopied < bytes)
        {
            CheckCancelled();
            uint64_t remainingBytes = bytes - bytesCopied;
            uint32_t ioBytes = static_cast<uint32_t>(std::min<uint64_t>(remainingBytes, kChunk));
            ReadAt(source, sourceOffset + bytesCopied, buffer.get(), ioBytes, L"source");
            WriteTarget(targetOffset + bytesCopied, buffer.get(), ioBytes);
            bytesCopied += ioBytes;
            m_progress.Add(ioBytes);
        }
    }

    void CopyEngine::CopyRawFromDisk(HANDLE sourceDisk, const PlannedPartition& partition)
    {
        CopySpan(sourceDisk, partition.src.offset, partition.targetOffset, partition.src.length);
    }

    void CopyEngine::CopyNtfsBitmap(const std::wstring& sourcePath, const PlannedPartition& partition)
    {
        wil::unique_hfile volume = OpenSourceVolumeForCopy(sourcePath);

        NTFS_VOLUME_DATA_BUFFER volumeData{};
        DWORD bytesReturned = 0;
        if (!DeviceIoControl(volume.get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                &volumeData, sizeof(volumeData), &bytesReturned, nullptr))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"FSCTL_GET_NTFS_VOLUME_DATA failed for " + sourcePath, lastError);
        }

        const uint64_t clusterSize = static_cast<uint64_t>(volumeData.BytesPerCluster);

        // Gather allocated runs from the volume bitmap, coalescing adjacent
        // and near-adjacent (< 1 MiB gap) runs into large sequential extents —
        // large I/O beats strictly-minimal I/O here.
        std::vector<Run> runs;
        {
            const uint32_t kBitmapChunk = 4 * 1024 * 1024;   // bits for ~32M clusters per call
            std::vector<uint8_t> bitmapBuffer(sizeof(VOLUME_BITMAP_BUFFER) + kBitmapChunk);
            uint64_t nextLcn = 0;
            bool moreChunks = true;
            uint64_t runStart = UINT64_MAX;
            uint64_t runLength = 0;

            auto flushRun = [&]()
            {
                if (runStart == UINT64_MAX) { return; }
                uint64_t byteOffset = runStart * clusterSize;
                uint64_t byteLength = runLength * clusterSize;
                if (!runs.empty())
                {
                    Run& lastRun = runs.back();
                    if (byteOffset - (lastRun.offset + lastRun.length) < kMiB)
                    {
                        lastRun.length = byteOffset + byteLength - lastRun.offset;   // bridge the small gap
                        runStart = UINT64_MAX;
                        runLength = 0;
                        return;
                    }
                }

                runs.push_back({ byteOffset, byteLength });
                runStart = UINT64_MAX;
                runLength = 0;
            };

            while (moreChunks)
            {
                CheckCancelled();
                STARTING_LCN_INPUT_BUFFER startingLcn{};
                startingLcn.StartingLcn.QuadPart = static_cast<LONGLONG>(nextLcn);
                DWORD bitmapBytes = 0;
                BOOL succeeded = DeviceIoControl(volume.get(), FSCTL_GET_VOLUME_BITMAP,
                    &startingLcn, sizeof(startingLcn),
                    bitmapBuffer.data(), static_cast<DWORD>(bitmapBuffer.size()), &bitmapBytes, nullptr);
                DWORD errorCode = succeeded ? ERROR_SUCCESS : GetLastError();
                if (!succeeded && errorCode != ERROR_MORE_DATA)
                {
                    ThrowWin32Error(ExitCode::CopyFailure, L"FSCTL_GET_VOLUME_BITMAP failed", errorCode);
                }

                moreChunks = !succeeded;   // ERROR_MORE_DATA -> more chunks follow

                auto* bitmap = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(bitmapBuffer.data());
                uint64_t baseCluster = static_cast<uint64_t>(bitmap->StartingLcn.QuadPart);
                uint64_t clusterCount = static_cast<uint64_t>(bitmap->BitmapSize.QuadPart);
                uint64_t usableBits = std::min<uint64_t>(clusterCount,
                    (bitmapBytes - offsetof(VOLUME_BITMAP_BUFFER, Buffer)) * 8ull);
                for (uint64_t bit = 0; bit < usableBits; ++bit)
                {
                    bool allocated = (bitmap->Buffer[bit >> 3] >> (bit & 7)) & 1;
                    uint64_t cluster = baseCluster + bit;
                    if (allocated)
                    {
                        if (runStart == UINT64_MAX) { runStart = cluster; runLength = 1; }
                        else if (cluster == runStart + runLength) { ++runLength; }
                        else { flushRun(); runStart = cluster; runLength = 1; }
                    }
                    else if (runStart != UINT64_MAX)
                    {
                        flushRun();
                    }
                }

                nextLcn = baseCluster + usableBits;
                if (usableBits == 0) { break; }
            }

            flushRun();
        }

        // Region 1: the first 1 MiB of the volume ($Boot and friends),
        // unconditionally — belt and suspenders under the bitmap copy.
        {
            uint64_t headBytes = std::min<uint64_t>(kMiB, partition.src.length);
            CopySpan(volume.get(), 0, partition.targetOffset, headBytes);
        }

        // Allocated cluster runs (skip what the head copy already covered).
        for (const Run& run : runs)
        {
            uint64_t offset = run.offset;
            uint64_t length = run.length;
            if (offset + length <= kMiB) { continue; }
            if (offset < kMiB)
            {
                length -= kMiB - offset;
                offset = kMiB;
            }

            CopySpan(volume.get(), offset, partition.targetOffset + offset, length);
        }

        // Region 2: the NTFS backup boot sector — the sector at NumberSectors
        // (the BPB total excludes it), past the last cluster and absent from
        // the bitmap. A mounted volume handle cannot read past NumberSectors
        // (EOF), but the backup is by definition a copy of the primary boot
        // sector, so replicate sector 0 into the backup position. Offsets are
        // source-volume-relative, so this lands correctly even when the target
        // partition is larger (--expand): the FS is still source-sized.
        {
            uint64_t backupBootOffset = static_cast<uint64_t>(volumeData.NumberSectors.QuadPart) * m_sectorSize;
            if (backupBootOffset + m_sectorSize <= partition.src.length)
            {
                auto sectorBuffer = AllocateAlignedBuffer(m_sectorSize);
                ReadAt(volume.get(), 0, sectorBuffer.get(), m_sectorSize, L"boot sector");
                WriteTarget(partition.targetOffset + backupBootOffset, sectorBuffer.get(), m_sectorSize);
            }
        }
    }
}
