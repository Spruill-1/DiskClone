#include "volumes.h"
#include "disks.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>

namespace DiskClone
{
    namespace
    {
        // FindFirstVolume handles have their own closer and INVALID_HANDLE_VALUE
        // semantics; composed from WIL's unique_any family rather than a
        // hand-rolled guard.
        using unique_hfind_volume = wil::unique_any_handle_invalid<decltype(&::FindVolumeClose), ::FindVolumeClose>;

        wil::unique_hfile OpenVolumeNoTrailingSlash(const std::wstring& guidPath, DWORD access)
        {
            std::wstring path = guidPath;
            if (!path.empty() && path.back() == L'\\') { path.pop_back(); }
            return wil::unique_hfile{ CreateFileW(path.c_str(), access,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr) };
        }

        // The volume device (\Device\HarddiskVolumeN) backing the in-use system
        // partition — the partition holding the boot files (ESP on UEFI, active
        // partition on BIOS) — as recorded by Windows setup/boot. This is how we
        // catch the split-disk configuration where the boot files live on a
        // different physical disk than C:\Windows.
        std::wstring SystemPartitionDeviceName()
        {
            wil::unique_hkey setupKey;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", 0, KEY_QUERY_VALUE, &setupKey) != ERROR_SUCCESS)
            {
                return L"";
            }

            wchar_t systemPartition[MAX_PATH]{};
            DWORD valueSize = sizeof(systemPartition);
            DWORD valueType = 0;
            LSTATUS status = RegQueryValueExW(setupKey.get(), L"SystemPartition", nullptr, &valueType,
                reinterpret_cast<LPBYTE>(systemPartition), &valueSize);
            if (status != ERROR_SUCCESS || valueType != REG_SZ) { return L""; }
            systemPartition[ARRAYSIZE(systemPartition) - 1] = L'\0';
            return systemPartition;
        }

        // \Device\HarddiskVolumeN for a \\?\Volume{...}\ path, via QueryDosDevice.
        std::wstring VolumeDeviceName(const std::wstring& guidPath)
        {
            // Strip the "\\?\" prefix and trailing backslash -> "Volume{...}"
            std::wstring volumeName = guidPath;
            if (volumeName.rfind(L"\\\\?\\", 0) == 0) { volumeName = volumeName.substr(4); }
            if (!volumeName.empty() && volumeName.back() == L'\\') { volumeName.pop_back(); }
            wchar_t deviceName[MAX_PATH]{};
            if (!QueryDosDeviceW(volumeName.c_str(), deviceName, ARRAYSIZE(deviceName))) { return L""; }
            return deviceName;
        }
    }

    std::vector<VolumeInfo> EnumerateVolumes()
    {
        std::vector<VolumeInfo> volumes;
        wchar_t volumeName[MAX_PATH];
        unique_hfind_volume findHandle{ FindFirstVolumeW(volumeName, MAX_PATH) };
        if (!findHandle) { return volumes; }

        do
        {
            VolumeInfo volume;
            volume.guidPath = volumeName;

            // Mount points / drive letters
            DWORD neededChars = 0;
            std::vector<wchar_t> pathsBuffer(MAX_PATH);
            if (GetVolumePathNamesForVolumeNameW(volumeName, pathsBuffer.data(),
                    static_cast<DWORD>(pathsBuffer.size()), &neededChars) ||
                (GetLastError() == ERROR_MORE_DATA &&
                 (pathsBuffer.resize(neededChars),
                  GetVolumePathNamesForVolumeNameW(volumeName, pathsBuffer.data(), neededChars, &neededChars))))
            {
                for (const wchar_t* mountPath = pathsBuffer.data(); *mountPath; mountPath += wcslen(mountPath) + 1)
                {
                    volume.mountPaths.emplace_back(mountPath);
                    if (!volume.letters.empty()) { volume.letters += L" "; }
                    std::wstring mountPoint = mountPath;
                    if (!mountPoint.empty() && mountPoint.back() == L'\\' && mountPoint.size() <= 3)
                    {
                        mountPoint.pop_back();
                    }

                    volume.letters += mountPoint;
                }
            }

            // Disk extents
            wil::unique_hfile volumeHandle = OpenVolumeNoTrailingSlash(volumeName, 0);
            if (volumeHandle)
            {
                alignas(VOLUME_DISK_EXTENTS)
                uint8_t extentsBuffer[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)]{};
                DWORD bytesReturned = 0;
                if (DeviceIoControl(volumeHandle.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                        nullptr, 0, extentsBuffer, sizeof(extentsBuffer), &bytesReturned, nullptr))
                {
                    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(extentsBuffer);
                    if (extents->NumberOfDiskExtents == 1)
                    {
                        volume.diskNumber = static_cast<int>(extents->Extents[0].DiskNumber);
                        volume.offset = static_cast<uint64_t>(extents->Extents[0].StartingOffset.QuadPart);
                        volume.length = static_cast<uint64_t>(extents->Extents[0].ExtentLength.QuadPart);
                    }
                    else if (extents->NumberOfDiskExtents > 1)
                    {
                        volume.multiExtent = true;
                    }
                }
            }

            volumes.push_back(std::move(volume));
        } while (FindNextVolumeW(findHandle.get(), volumeName, MAX_PATH));
        return volumes;
    }

    std::vector<VolumeInfo> VolumesOnDisk(const std::vector<VolumeInfo>& all, int diskNumber)
    {
        std::vector<VolumeInfo> result;
        for (const auto& volume : all)
        {
            if (volume.diskNumber == diskNumber)
            {
                result.push_back(volume);
            }
        }

        return result;
    }

    std::wstring SystemVolumeGuidPath()
    {
        wchar_t windowsDirectory[MAX_PATH];
        if (!GetWindowsDirectoryW(windowsDirectory, MAX_PATH))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::SafetyRefusal, L"GetWindowsDirectoryW failed", lastError);
        }

        // Mount point of the volume containing the Windows directory, e.g. "C:\"
        std::wstring rootPath = windowsDirectory;
        size_t firstSlash = rootPath.find(L'\\');
        rootPath = rootPath.substr(0, firstSlash + 1);
        wchar_t volumeName[MAX_PATH];
        if (!GetVolumeNameForVolumeMountPointW(rootPath.c_str(), volumeName, MAX_PATH))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::SafetyRefusal, L"GetVolumeNameForVolumeMountPointW failed", lastError);
        }

        return volumeName;
    }

    void AnnotateSystemAndPagefileDisks(std::vector<DiskInfo>& disks)
    {
        auto volumes = EnumerateVolumes();
        std::wstring systemVolume = SystemVolumeGuidPath();
        std::wstring bootPartitionDevice = SystemPartitionDeviceName();

        // Pagefile detection: probe every real mount path of the volume itself —
        // never a substring of a folder-mount path (which would test the HOST
        // volume) — plus the volume GUID path directly, which also covers
        // pagefiles on letterless volumes.
        auto hasPagefile = [](const VolumeInfo& volume)
        {
            std::vector<std::wstring> probeRoots = volume.mountPaths;
            probeRoots.push_back(volume.guidPath);
            for (const auto& probeRoot : probeRoots)
            {
                for (const wchar_t* pagefileName : { L"pagefile.sys", L"swapfile.sys" })
                {
                    DWORD attributes = GetFileAttributesW((probeRoot + pagefileName).c_str());
                    if (attributes != INVALID_FILE_ATTRIBUTES) { return true; }
                }
            }

            return false;
        };

        for (auto& disk : disks)
        {
            for (const auto& volume : volumes)
            {
                if (volume.diskNumber != disk.number) { continue; }
                if (volume.guidPath == systemVolume) { disk.isSystemDisk = true; }
                if (!bootPartitionDevice.empty() && VolumeDeviceName(volume.guidPath) == bootPartitionDevice)
                {
                    disk.isBootDisk = true;
                }

                if (hasPagefile(volume)) { disk.hasPagefile = true; }
            }
        }
    }

    FsKind SniffFilesystem(HANDLE disk, uint64_t partitionOffset, uint32_t sectorSize)
    {
        // Read one logical sector at the partition start through the disk handle.
        auto sectorBuffer = AllocateAlignedBuffer(std::max<uint32_t>(sectorSize, 4096));
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(partitionOffset & 0xFFFFFFFF);
        overlapped.OffsetHigh = static_cast<DWORD>(partitionOffset >> 32);
        DWORD bytesRead = 0;
        if (!ReadFile(disk, sectorBuffer.get(), sectorSize, &bytesRead, &overlapped) || bytesRead < 512)
        {
            return FsKind::Unknown;
        }

        const uint8_t* sector = sectorBuffer.get();
        if (memcmp(sector + 3, "NTFS    ", 8) == 0) { return FsKind::Ntfs; }
        if (memcmp(sector + 3, "-FVE-FS-", 8) == 0) { return FsKind::BitLocker; }
        // ReFS: no BPB OEM at offset 3; FileSystemName "ReFS" at offset 3 in
        // the Volume Boot Record per the ReFS on-disk VBR definition.
        if (memcmp(sector + 3, "ReFS    ", 8) == 0) { return FsKind::Refs; }
        // FAT12/16 signature at 54, FAT32 at 82.
        if (memcmp(sector + 82, "FAT32   ", 8) == 0) { return FsKind::Fat; }
        if (memcmp(sector + 54, "FAT16   ", 8) == 0 || memcmp(sector + 54, "FAT12   ", 8) == 0 ||
            memcmp(sector + 54, "FAT     ", 8) == 0)
        {
            return FsKind::Fat;
        }

        return FsKind::Unknown;
    }

    const wchar_t* FsKindName(FsKind kind)
    {
        switch (kind)
        {
        case FsKind::Ntfs: return L"NTFS";
        case FsKind::Fat: return L"FAT";
        case FsKind::BitLocker: return L"BitLocker";
        case FsKind::Refs: return L"ReFS";
        default: return L"?";
        }
    }

    wil::unique_hfile LockAndDismountVolume(const std::wstring& guidPath)
    {
        wil::unique_hfile volumeHandle = OpenVolumeNoTrailingSlash(guidPath, GENERIC_READ | GENERIC_WRITE);
        if (!volumeHandle)
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"cannot open volume " + guidPath, lastError);
        }

        DWORD bytesReturned = 0;
        if (!DeviceIoControl(volumeHandle.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure,
                L"FSCTL_LOCK_VOLUME failed for " + guidPath + L" (volume in use)", lastError);
        }

        if (!DeviceIoControl(volumeHandle.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"FSCTL_DISMOUNT_VOLUME failed for " + guidPath, lastError);
        }

        return volumeHandle;
    }
}
