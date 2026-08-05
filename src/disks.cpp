#include "disks.h"
#include "volumes.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>

namespace dc {

// {C12A7328-F81F-11D2-BA4B-00A0C93EC93B}
const GUID kEspType = { 0xC12A7328, 0xF81F, 0x11D2, {0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B} };
// {E3C9E316-0B5C-4DB8-817D-F92DF00215AE}
const GUID kMsrType = { 0xE3C9E316, 0x0B5C, 0x4DB8, {0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE} };
// {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}
const GUID kBasicDataType = { 0xEBD0A0A2, 0xB9E5, 0x4433, {0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7} };
// {DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}
const GUID kWinReType = { 0xDE94BBA4, 0x06D1, 0x4D40, {0xA1,0x6A,0xBF,0xD5,0x01,0x79,0xD6,0xAC} };
// {5808C8AA-7E8F-42E0-85D2-E1E90434CFB3}
const GUID kLdmMetadataType = { 0x5808C8AA, 0x7E8F, 0x42E0, {0x85,0xD2,0xE1,0xE9,0x04,0x34,0xCF,0xB3} };
// {AF9B60A0-1431-4F62-BC68-3311714A69AD}
const GUID kLdmDataType = { 0xAF9B60A0, 0x1431, 0x4F62, {0xBC,0x68,0x33,0x11,0x71,0x4A,0x69,0xAD} };
// {E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D}
const GUID kStorageSpacesType = { 0xE75CAF8F, 0xF680, 0x4CEE, {0xAF,0xA3,0xB0,0x01,0xE5,0x6E,0xFC,0x2D} };

unique_handle OpenPhysicalDisk(int number, DWORD access, bool optional) {
    std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
    HANDLE h = CreateFileW(path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (optional) return unique_handle{};
        ThrowWin32(ExitCode::SafetyRefusal, L"cannot open " + path);
    }
    return unique_handle(h);
}

static std::wstring BusTypeName(STORAGE_BUS_TYPE t) {
    switch (t) {
    case BusTypeAta: return L"ATA";
    case BusTypeSata: return L"SATA";
    case BusTypeScsi: return L"SCSI";
    case BusTypeNvme: return L"NVMe";
    case BusTypeUsb: return L"USB";
    case BusTypeSd: return L"SD";
    case BusTypeMmc: return L"MMC";
    case BusTypeRAID: return L"RAID";
    case BusTypeVirtual: return L"Virtual";
    case BusTypeFileBackedVirtual: return L"VHD";
    case BusTypeSas: return L"SAS";
    case BusTypeSpaces: return L"Spaces";
    default: return L"Other";
    }
}

static std::wstring TrimAscii(const char* s, size_t maxLen) {
    if (!s) return L"";
    std::string a(s, strnlen_s(s, maxLen));
    while (!a.empty() && (a.back() == ' ' || a.back() == '\0')) a.pop_back();
    size_t start = a.find_first_not_of(' ');
    if (start == std::string::npos) return L"";
    return std::wstring(a.begin() + start, a.end());
}

static void QueryDeviceProperties(HANDLE h, DiskInfo& d) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<uint8_t> buf(4096);
    DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                        buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr) &&
        ret >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        d.busType = BusTypeName(desc->BusType);
        const char* base = reinterpret_cast<const char*>(buf.data());
        // Offsets come from the driver; validate against the bytes actually
        // returned before touching them (a truncated or hostile descriptor
        // must not cause an out-of-bounds read).
        auto stringAt = [&](DWORD off) -> std::wstring {
            if (off == 0 || off >= ret) return L"";
            return TrimAscii(base + off, ret - off);
        };
        std::wstring vendor = stringAt(desc->VendorIdOffset);
        std::wstring product = stringAt(desc->ProductIdOffset);
        d.model = vendor.empty() ? product : vendor + L" " + product;
        d.serial = stringAt(desc->SerialNumberOffset);
    }

    q.PropertyId = StorageAccessAlignmentProperty;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR align{};
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                        &align, sizeof(align), &ret, nullptr)) {
        if (align.BytesPerLogicalSector) d.logicalSectorSize = align.BytesPerLogicalSector;
        if (align.BytesPerPhysicalSector) d.physicalSectorSize = align.BytesPerPhysicalSector;
    }
}

static void QueryGeometry(HANDLE h, DiskInfo& d) {
    DISK_GEOMETRY_EX geo{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                         &geo, sizeof(geo), &ret, nullptr))
        ThrowWin32(ExitCode::SafetyRefusal, L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed");
    d.size = static_cast<uint64_t>(geo.DiskSize.QuadPart);
    if (geo.Geometry.BytesPerSector) d.logicalSectorSize = geo.Geometry.BytesPerSector;
}

static void QueryAttributes(HANDLE h, DiskInfo& d) {
    GET_DISK_ATTRIBUTES attrs{};
    DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0,
                        &attrs, sizeof(attrs), &ret, nullptr))
        d.offline = (attrs.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;
}

static void QueryLayout(HANDLE h, DiskInfo& d) {
    // Variable-size structure: grow until it fits. A failure here is NOT the
    // same as a RAW disk (RAW disks succeed with PARTITION_STYLE_RAW); leave
    // layoutKnown=false so the planner refuses to trust this disk.
    std::vector<uint8_t> buf(sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 127 * sizeof(PARTITION_INFORMATION_EX));
    DWORD ret = 0;
    BOOL ok = FALSE;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ok = DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                             buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr);
        if (ok) break;
        DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER && err != ERROR_MORE_DATA) break;
        buf.resize(buf.size() * 2);
    }
    if (!ok) {
        d.style = PARTITION_STYLE_RAW;
        return;
    }
    d.layoutKnown = true;
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
    d.style = static_cast<PARTITION_STYLE>(layout->PartitionStyle);
    if (d.style == PARTITION_STYLE_GPT) d.gptDiskId = layout->Gpt.DiskId;
    if (d.style == PARTITION_STYLE_MBR) d.mbrSignature = layout->Mbr.Signature;

    for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX& p = layout->PartitionEntry[i];
        if (p.PartitionStyle == PARTITION_STYLE_MBR &&
            (p.Mbr.PartitionType == PARTITION_ENTRY_UNUSED))
            continue;
        if (p.PartitionStyle == PARTITION_STYLE_GPT && p.PartitionLength.QuadPart == 0)
            continue;
        PartitionInfo pi;
        pi.number = static_cast<int>(p.PartitionNumber);
        pi.offset = static_cast<uint64_t>(p.StartingOffset.QuadPart);
        pi.length = static_cast<uint64_t>(p.PartitionLength.QuadPart);
        if (p.PartitionStyle == PARTITION_STYLE_GPT) {
            pi.gptType = p.Gpt.PartitionType;
            pi.gptId = p.Gpt.PartitionId;
            pi.gptAttributes = p.Gpt.Attributes;
            // Name is WCHAR[36] and NOT guaranteed NUL-terminated on disk;
            // an unbounded copy would read past the field.
            pi.gptName.assign(p.Gpt.Name, wcsnlen(p.Gpt.Name, ARRAYSIZE(p.Gpt.Name)));
        } else if (p.PartitionStyle == PARTITION_STYLE_MBR) {
            pi.mbrType = p.Mbr.PartitionType;
            pi.mbrActive = p.Mbr.BootIndicator != FALSE;
            pi.mbrHiddenSectors = p.Mbr.HiddenSectors;
        }
        if (pi.length > 0)
            d.partitions.push_back(pi);
    }
    std::sort(d.partitions.begin(), d.partitions.end(),
        [](const PartitionInfo& a, const PartitionInfo& b) { return a.offset < b.offset; });
}

std::optional<DiskInfo> QueryDisk(int number) {
    unique_handle h = OpenPhysicalDisk(number, GENERIC_READ, /*optional=*/true);
    if (!h.valid()) return std::nullopt;
    DiskInfo d;
    d.number = number;
    QueryGeometry(h.get(), d);
    QueryDeviceProperties(h.get(), d);
    QueryAttributes(h.get(), d);
    QueryLayout(h.get(), d);
    return d;
}

std::vector<DiskInfo> EnumerateDisks() {
    std::vector<DiskInfo> disks;
    int misses = 0;
    for (int n = 0; n < 256 && misses < 8; ++n) {
        auto d = QueryDisk(n);
        if (!d) { ++misses; continue; }
        misses = 0;
        disks.push_back(std::move(*d));
    }
    AnnotateSystemAndPagefileDisks(disks);
    return disks;
}

bool IsDynamicOrStorageSpaces(const DiskInfo& d) {
    for (const auto& p : d.partitions) {
        if (d.style == PARTITION_STYLE_MBR && p.mbrType == 0x42) return true;
        if (d.style == PARTITION_STYLE_GPT &&
            (IsEqualGUID(p.gptType, kLdmMetadataType) ||
             IsEqualGUID(p.gptType, kLdmDataType) ||
             IsEqualGUID(p.gptType, kStorageSpacesType)))
            return true;
    }
    return d.busType == L"Spaces";
}

void SetDiskOffline(HANDLE disk, bool offline) {
    SET_DISK_ATTRIBUTES attrs{};
    attrs.Version = sizeof(attrs);
    attrs.Persist = TRUE;
    attrs.Attributes = offline ? DISK_ATTRIBUTE_OFFLINE : 0;
    attrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
    DWORD ret = 0;
    if (!DeviceIoControl(disk, IOCTL_DISK_SET_DISK_ATTRIBUTES, &attrs, sizeof(attrs),
                         nullptr, 0, &ret, nullptr))
        ThrowWin32(ExitCode::CopyFailure,
            offline ? L"failed to set disk offline" : L"failed to set disk online");
}

void DeleteDriveLayout(HANDLE disk) {
    DWORD ret = 0;
    // Fails with ERROR_INVALID_FUNCTION on already-RAW disks; that is fine.
    DeviceIoControl(disk, IOCTL_DISK_DELETE_DRIVE_LAYOUT, nullptr, 0, nullptr, 0, &ret, nullptr);
}

void CreateDiskStyle(HANDLE disk, PARTITION_STYLE style, const GUID& gptDiskId, uint32_t mbrSignature) {
    CREATE_DISK cd{};
    if (style == PARTITION_STYLE_GPT) {
        cd.PartitionStyle = PARTITION_STYLE_GPT;
        cd.Gpt.DiskId = gptDiskId;
        cd.Gpt.MaxPartitionCount = 128;
    } else {
        cd.PartitionStyle = PARTITION_STYLE_MBR;
        cd.Mbr.Signature = mbrSignature;
    }
    DWORD ret = 0;
    if (!DeviceIoControl(disk, IOCTL_DISK_CREATE_DISK, &cd, sizeof(cd), nullptr, 0, &ret, nullptr))
        ThrowWin32(ExitCode::CopyFailure, L"IOCTL_DISK_CREATE_DISK failed");
}

void SetDriveLayout(HANDLE disk, const std::vector<uint8_t>& layoutBuffer) {
    DWORD ret = 0;
    if (!DeviceIoControl(disk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
                         const_cast<uint8_t*>(layoutBuffer.data()),
                         static_cast<DWORD>(layoutBuffer.size()), nullptr, 0, &ret, nullptr))
        ThrowWin32(ExitCode::CopyFailure, L"IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed");
}

void UpdateDiskProperties(HANDLE disk) {
    DWORD ret = 0;
    DeviceIoControl(disk, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &ret, nullptr);
}

void RevalidateDiskIdentity(const DiskInfo& expected, const wchar_t* role) {
    auto now = QueryDisk(expected.number);
    auto refuse = [&](const std::wstring& what) {
        throw Error(ExitCode::SafetyRefusal,
            std::wstring(role) + L" disk " + std::to_wstring(expected.number) +
            L" changed since it was validated (" + what +
            L"); disks may have been added or removed — re-run diskclone");
    };
    if (!now) refuse(L"no longer present");
    if (now->serial != expected.serial) refuse(L"serial number differs");
    if (now->model != expected.model) refuse(L"model differs");
    if (now->size != expected.size) refuse(L"size differs");
    if (now->logicalSectorSize != expected.logicalSectorSize) refuse(L"sector size differs");
    if (now->style != expected.style) refuse(L"partition style differs");
    if (expected.style == PARTITION_STYLE_GPT &&
        !IsEqualGUID(now->gptDiskId, expected.gptDiskId)) refuse(L"GPT disk GUID differs");
    if (expected.style == PARTITION_STYLE_MBR &&
        now->mbrSignature != expected.mbrSignature) refuse(L"MBR signature differs");
}

} // namespace dc
