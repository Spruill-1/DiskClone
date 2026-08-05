#include "volumes.h"
#include "disks.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>

namespace dc {

static unique_handle OpenVolumeNoTrailingSlash(const std::wstring& guidPath, DWORD access) {
    std::wstring path = guidPath;
    if (!path.empty() && path.back() == L'\\') path.pop_back();
    HANDLE h = CreateFileW(path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    return unique_handle(h);
}

namespace {
struct VolumeFindGuard {
    HANDLE h;
    ~VolumeFindGuard() { if (h != INVALID_HANDLE_VALUE) FindVolumeClose(h); }
};
} // namespace

std::vector<VolumeInfo> EnumerateVolumes() {
    std::vector<VolumeInfo> vols;
    wchar_t name[MAX_PATH];
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return vols;
    VolumeFindGuard guard{ find };

    do {
        VolumeInfo v;
        v.guidPath = name;

        // Mount points / drive letters
        DWORD needed = 0;
        std::vector<wchar_t> paths(MAX_PATH);
        if (GetVolumePathNamesForVolumeNameW(name, paths.data(),
                static_cast<DWORD>(paths.size()), &needed) ||
            (GetLastError() == ERROR_MORE_DATA &&
             (paths.resize(needed),
              GetVolumePathNamesForVolumeNameW(name, paths.data(), needed, &needed)))) {
            for (const wchar_t* p = paths.data(); *p; p += wcslen(p) + 1) {
                v.mountPaths.emplace_back(p);
                if (!v.letters.empty()) v.letters += L" ";
                std::wstring mp = p;
                if (!mp.empty() && mp.back() == L'\\' && mp.size() <= 3) mp.pop_back();
                v.letters += mp;
            }
        }

        // Disk extents
        unique_handle h = OpenVolumeNoTrailingSlash(name, 0);
        if (h.valid()) {
            alignas(VOLUME_DISK_EXTENTS)
            uint8_t buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)]{};
            DWORD ret = 0;
            if (DeviceIoControl(h.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                    nullptr, 0, buf, sizeof(buf), &ret, nullptr)) {
                auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
                if (ext->NumberOfDiskExtents == 1) {
                    v.diskNumber = static_cast<int>(ext->Extents[0].DiskNumber);
                    v.offset = static_cast<uint64_t>(ext->Extents[0].StartingOffset.QuadPart);
                    v.length = static_cast<uint64_t>(ext->Extents[0].ExtentLength.QuadPart);
                } else if (ext->NumberOfDiskExtents > 1) {
                    v.multiExtent = true;
                }
            }
        }
        vols.push_back(std::move(v));
    } while (FindNextVolumeW(find, name, MAX_PATH));
    return vols;
}

std::vector<VolumeInfo> VolumesOnDisk(const std::vector<VolumeInfo>& all, int diskNumber) {
    std::vector<VolumeInfo> out;
    for (const auto& v : all)
        if (v.diskNumber == diskNumber)
            out.push_back(v);
    return out;
}

std::wstring SystemVolumeGuidPath() {
    wchar_t windir[MAX_PATH];
    if (!GetWindowsDirectoryW(windir, MAX_PATH))
        ThrowWin32(ExitCode::SafetyRefusal, L"GetWindowsDirectoryW failed");
    // Mount point of the volume containing the Windows directory, e.g. "C:\"
    std::wstring root = windir;
    size_t slash = root.find(L'\\');
    root = root.substr(0, slash + 1);
    wchar_t volName[MAX_PATH];
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volName, MAX_PATH))
        ThrowWin32(ExitCode::SafetyRefusal, L"GetVolumeNameForVolumeMountPointW failed");
    return volName;
}

// The volume device (\Device\HarddiskVolumeN) backing the in-use system
// partition — the partition holding the boot files (ESP on UEFI, active
// partition on BIOS) — as recorded by Windows setup/boot.
static std::wstring SystemPartitionDeviceName() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[MAX_PATH]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(key, L"SystemPartition", nullptr, &type,
        reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ) return L"";
    buf[ARRAYSIZE(buf) - 1] = L'\0';
    return buf;
}

// \Device\HarddiskVolumeN for a \\?\Volume{...}\ path, via QueryDosDevice.
static std::wstring VolumeDeviceName(const std::wstring& guidPath) {
    // Strip "\\?\" prefix and trailing backslash -> "Volume{...}"
    std::wstring name = guidPath;
    if (name.rfind(L"\\\\?\\", 0) == 0) name = name.substr(4);
    if (!name.empty() && name.back() == L'\\') name.pop_back();
    wchar_t target[MAX_PATH]{};
    if (!QueryDosDeviceW(name.c_str(), target, ARRAYSIZE(target)))
        return L"";
    return target;
}

void AnnotateSystemAndPagefileDisks(std::vector<DiskInfo>& disks) {
    auto vols = EnumerateVolumes();
    std::wstring sysVol = SystemVolumeGuidPath();
    std::wstring bootPartDevice = SystemPartitionDeviceName();

    // Pagefile detection: probe every real mount path of the volume itself —
    // never a substring of a folder-mount path (which would test the HOST
    // volume) — plus the volume GUID path directly, which also covers
    // pagefiles on letterless volumes.
    auto hasPagefile = [](const VolumeInfo& v) {
        std::vector<std::wstring> roots = v.mountPaths;
        roots.push_back(v.guidPath);
        for (const auto& root : roots) {
            for (const wchar_t* pf : { L"pagefile.sys", L"swapfile.sys" }) {
                DWORD attrs = GetFileAttributesW((root + pf).c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES) return true;
            }
        }
        return false;
    };

    for (auto& d : disks) {
        for (const auto& v : vols) {
            if (v.diskNumber != d.number) continue;
            if (v.guidPath == sysVol) d.isSystemDisk = true;
            if (!bootPartDevice.empty() && VolumeDeviceName(v.guidPath) == bootPartDevice)
                d.isBootDisk = true;
            if (hasPagefile(v)) d.hasPagefile = true;
        }
    }
}

FsKind SniffFilesystem(HANDLE disk, uint64_t partitionOffset, uint32_t sectorSize) {
    // Read one logical sector at the partition start through the disk handle.
    AlignedBuffer buf(std::max<uint32_t>(sectorSize, 4096));
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(partitionOffset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(partitionOffset >> 32);
    DWORD read = 0;
    if (!ReadFile(disk, buf.data(), sectorSize, &read, &ov) || read < 512)
        return FsKind::Unknown;
    const uint8_t* s = buf.data();
    if (memcmp(s + 3, "NTFS    ", 8) == 0) return FsKind::Ntfs;
    if (memcmp(s + 3, "-FVE-FS-", 8) == 0) return FsKind::BitLocker;
    // ReFS: no BPB OEM at offset 3; FileSystemName "ReFS" at offset 3 in the
    // Volume Boot Record per the ReFS on-disk VBR definition.
    if (memcmp(s + 3, "ReFS    ", 8) == 0) return FsKind::Refs;
    // FAT12/16 signature at 54, FAT32 at 82.
    if (memcmp(s + 82, "FAT32   ", 8) == 0) return FsKind::Fat;
    if (memcmp(s + 54, "FAT16   ", 8) == 0 || memcmp(s + 54, "FAT12   ", 8) == 0 ||
        memcmp(s + 54, "FAT     ", 8) == 0)
        return FsKind::Fat;
    return FsKind::Unknown;
}

const wchar_t* FsKindName(FsKind k) {
    switch (k) {
    case FsKind::Ntfs: return L"NTFS";
    case FsKind::Fat: return L"FAT";
    case FsKind::BitLocker: return L"BitLocker";
    case FsKind::Refs: return L"ReFS";
    default: return L"?";
    }
}

unique_handle LockAndDismountVolume(const std::wstring& guidPath) {
    unique_handle h = OpenVolumeNoTrailingSlash(guidPath, GENERIC_READ | GENERIC_WRITE);
    if (!h.valid())
        ThrowWin32(ExitCode::CopyFailure, L"cannot open volume " + guidPath);
    DWORD ret = 0;
    if (!DeviceIoControl(h.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ret, nullptr))
        ThrowWin32(ExitCode::CopyFailure, L"FSCTL_LOCK_VOLUME failed for " + guidPath +
            L" (volume in use)");
    if (!DeviceIoControl(h.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &ret, nullptr))
        ThrowWin32(ExitCode::CopyFailure, L"FSCTL_DISMOUNT_VOLUME failed for " + guidPath);
    return h;
}

} // namespace dc
