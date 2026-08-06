#include "disks.h"
#include "volumes.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>

namespace DiskClone
{
    // {C12A7328-F81F-11D2-BA4B-00A0C93EC93B}
    const GUID kEspType = { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };
    // {E3C9E316-0B5C-4DB8-817D-F92DF00215AE}
    const GUID kMsrType = { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } };
    // {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}
    const GUID kBasicDataType = { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } };
    // {DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}
    const GUID kWinReType = { 0xDE94BBA4, 0x06D1, 0x4D40, { 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC } };
    // {5808C8AA-7E8F-42E0-85D2-E1E90434CFB3}
    const GUID kLdmMetadataType = { 0x5808C8AA, 0x7E8F, 0x42E0, { 0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3 } };
    // {AF9B60A0-1431-4F62-BC68-3311714A69AD}
    const GUID kLdmDataType = { 0xAF9B60A0, 0x1431, 0x4F62, { 0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD } };
    // {E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D}
    const GUID kStorageSpacesType = { 0xE75CAF8F, 0xF680, 0x4CEE, { 0xAF, 0xA3, 0xB0, 0x01, 0xE5, 0x6E, 0xFC, 0x2D } };

    wil::unique_hfile OpenPhysicalDisk(int number, DWORD access, bool optional)
    {
        std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
        wil::unique_hfile diskHandle{ CreateFileW(path.c_str(), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr) };
        if (!diskHandle)
        {
            if (optional) { return wil::unique_hfile{}; }
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::SafetyRefusal, L"cannot open " + path, lastError);
        }

        return diskHandle;
    }

    namespace
    {
        std::wstring BusTypeName(STORAGE_BUS_TYPE busType)
        {
            switch (busType)
            {
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

        // Widen and trim a fixed-length ASCII identity string from a storage
        // descriptor. These arrive space-padded and are not guaranteed to be
        // terminated within the region the driver claims.
        std::wstring TrimAscii(const char* text, size_t maxLength)
        {
            if (!text) { return L""; }
            std::string ascii(text, strnlen_s(text, maxLength));
            while (!ascii.empty() && (ascii.back() == ' ' || ascii.back() == '\0'))
            {
                ascii.pop_back();
            }

            size_t firstNonSpace = ascii.find_first_not_of(' ');
            if (firstNonSpace == std::string::npos) { return L""; }
            return std::wstring(ascii.begin() + firstNonSpace, ascii.end());
        }

        void QueryDeviceProperties(HANDLE diskHandle, DiskInfo& disk)
        {
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceProperty;
            query.QueryType = PropertyStandardQuery;
            std::vector<uint8_t> descriptorBuffer(4096);
            DWORD bytesReturned = 0;
            if (DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                    descriptorBuffer.data(), static_cast<DWORD>(descriptorBuffer.size()), &bytesReturned, nullptr) &&
                bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
            {
                auto* descriptor = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(descriptorBuffer.data());
                disk.busType = BusTypeName(descriptor->BusType);
                const char* descriptorBase = reinterpret_cast<const char*>(descriptorBuffer.data());

                // Offsets come from the driver; validate against the bytes
                // actually returned before touching them (a truncated or
                // hostile descriptor must not cause an out-of-bounds read).
                auto stringAt = [&](DWORD offset) -> std::wstring
                {
                    if (offset == 0 || offset >= bytesReturned) { return L""; }
                    return TrimAscii(descriptorBase + offset, bytesReturned - offset);
                };
                std::wstring vendor = stringAt(descriptor->VendorIdOffset);
                std::wstring product = stringAt(descriptor->ProductIdOffset);
                disk.model = vendor.empty() ? product : vendor + L" " + product;
                disk.serial = stringAt(descriptor->SerialNumberOffset);
            }

            query.PropertyId = StorageAccessAlignmentProperty;
            STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignmentDescriptor{};
            if (DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                    &alignmentDescriptor, sizeof(alignmentDescriptor), &bytesReturned, nullptr))
            {
                if (alignmentDescriptor.BytesPerLogicalSector) { disk.logicalSectorSize = alignmentDescriptor.BytesPerLogicalSector; }
                if (alignmentDescriptor.BytesPerPhysicalSector) { disk.physicalSectorSize = alignmentDescriptor.BytesPerPhysicalSector; }
            }
        }

        void QueryGeometry(HANDLE diskHandle, DiskInfo& disk)
        {
            DISK_GEOMETRY_EX geometry{};
            DWORD bytesReturned = 0;
            if (!DeviceIoControl(diskHandle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                    &geometry, sizeof(geometry), &bytesReturned, nullptr))
            {
                const DWORD lastError = GetLastError();
                ThrowWin32Error(ExitCode::SafetyRefusal, L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed", lastError);
            }

            disk.size = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
            if (geometry.Geometry.BytesPerSector) { disk.logicalSectorSize = geometry.Geometry.BytesPerSector; }
        }

        void QueryAttributes(HANDLE diskHandle, DiskInfo& disk)
        {
            GET_DISK_ATTRIBUTES attributes{};
            DWORD bytesReturned = 0;
            if (DeviceIoControl(diskHandle, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0,
                    &attributes, sizeof(attributes), &bytesReturned, nullptr))
            {
                disk.offline = (attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;
            }
        }

        void QueryLayout(HANDLE diskHandle, DiskInfo& disk)
        {
            // Variable-size structure: grow until it fits. A failure here is NOT
            // the same as a RAW disk (RAW disks succeed with PARTITION_STYLE_RAW);
            // layoutKnown stays false so the planner refuses to trust this disk.
            std::vector<uint8_t> layoutBuffer(sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 127 * sizeof(PARTITION_INFORMATION_EX));
            DWORD bytesReturned = 0;
            BOOL succeeded = FALSE;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                succeeded = DeviceIoControl(diskHandle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                    layoutBuffer.data(), static_cast<DWORD>(layoutBuffer.size()), &bytesReturned, nullptr);
                if (succeeded) { break; }
                DWORD errorCode = GetLastError();
                if (errorCode != ERROR_INSUFFICIENT_BUFFER && errorCode != ERROR_MORE_DATA) { break; }
                layoutBuffer.resize(layoutBuffer.size() * 2);
            }

            if (!succeeded)
            {
                disk.style = PARTITION_STYLE_RAW;
                return;
            }

            disk.layoutKnown = true;

            auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuffer.data());
            disk.style = static_cast<PARTITION_STYLE>(layout->PartitionStyle);
            if (disk.style == PARTITION_STYLE_GPT) { disk.gptDiskId = layout->Gpt.DiskId; }
            if (disk.style == PARTITION_STYLE_MBR) { disk.mbrSignature = layout->Mbr.Signature; }

            for (DWORD i = 0; i < layout->PartitionCount; ++i)
            {
                const PARTITION_INFORMATION_EX& entry = layout->PartitionEntry[i];
                if (entry.PartitionStyle == PARTITION_STYLE_MBR && entry.Mbr.PartitionType == PARTITION_ENTRY_UNUSED)
                {
                    continue;
                }

                if (entry.PartitionStyle == PARTITION_STYLE_GPT && entry.PartitionLength.QuadPart == 0)
                {
                    continue;
                }

                PartitionInfo partition;
                partition.number = static_cast<int>(entry.PartitionNumber);
                partition.offset = static_cast<uint64_t>(entry.StartingOffset.QuadPart);
                partition.length = static_cast<uint64_t>(entry.PartitionLength.QuadPart);
                if (entry.PartitionStyle == PARTITION_STYLE_GPT)
                {
                    partition.gptType = entry.Gpt.PartitionType;
                    partition.gptId = entry.Gpt.PartitionId;
                    partition.gptAttributes = entry.Gpt.Attributes;

                    // Name is WCHAR[36] and NOT guaranteed NUL-terminated on
                    // disk; an unbounded copy would read past the field.
                    partition.gptName.assign(entry.Gpt.Name, wcsnlen(entry.Gpt.Name, ARRAYSIZE(entry.Gpt.Name)));
                }
                else if (entry.PartitionStyle == PARTITION_STYLE_MBR)
                {
                    partition.mbrType = entry.Mbr.PartitionType;
                    partition.mbrActive = entry.Mbr.BootIndicator != FALSE;
                    partition.mbrHiddenSectors = entry.Mbr.HiddenSectors;
                }

                if (partition.length > 0)
                {
                    disk.partitions.push_back(partition);
                }
            }

            std::sort(disk.partitions.begin(), disk.partitions.end(),
                [](const PartitionInfo& a, const PartitionInfo& b) { return a.offset < b.offset; });
        }
    }

    std::optional<DiskInfo> QueryDisk(int number)
    {
        wil::unique_hfile diskHandle = OpenPhysicalDisk(number, GENERIC_READ, /*optional=*/true);
        if (!diskHandle) { return std::nullopt; }

        DiskInfo disk;
        disk.number = number;
        QueryGeometry(diskHandle.get(), disk);
        QueryDeviceProperties(diskHandle.get(), disk);
        QueryAttributes(diskHandle.get(), disk);
        QueryLayout(diskHandle.get(), disk);
        return disk;
    }

    std::vector<DiskInfo> EnumerateDisks()
    {
        std::vector<DiskInfo> disks;
        int consecutiveMisses = 0;
        for (int diskNumber = 0; diskNumber < 256 && consecutiveMisses < 8; ++diskNumber)
        {
            auto disk = QueryDisk(diskNumber);
            if (!disk)
            {
                ++consecutiveMisses;
                continue;
            }

            consecutiveMisses = 0;
            disks.push_back(std::move(*disk));
        }

        AnnotateSystemAndPagefileDisks(disks);
        return disks;
    }

    bool IsDynamicOrStorageSpaces(const DiskInfo& disk)
    {
        for (const auto& partition : disk.partitions)
        {
            if (disk.style == PARTITION_STYLE_MBR && partition.mbrType == 0x42) { return true; }
            if (disk.style == PARTITION_STYLE_GPT &&
                (IsEqualGUID(partition.gptType, kLdmMetadataType) ||
                 IsEqualGUID(partition.gptType, kLdmDataType) ||
                 IsEqualGUID(partition.gptType, kStorageSpacesType)))
            {
                return true;
            }
        }

        return disk.busType == L"Spaces";
    }

    void RevalidateDiskIdentity(const DiskInfo& expected, const wchar_t* role)
    {
        auto current = QueryDisk(expected.number);
        auto refuse = [&](const std::wstring& what)
        {
            throw Error(ExitCode::SafetyRefusal,
                std::wstring(role) + L" disk " + std::to_wstring(expected.number) +
                L" changed since it was validated (" + what +
                L"); disks may have been added or removed — re-run diskclone");
        };
        if (!current) { refuse(L"no longer present"); }
        if (current->serial != expected.serial) { refuse(L"serial number differs"); }
        if (current->model != expected.model) { refuse(L"model differs"); }
        if (current->size != expected.size) { refuse(L"size differs"); }
        if (current->logicalSectorSize != expected.logicalSectorSize) { refuse(L"sector size differs"); }
        if (current->style != expected.style) { refuse(L"partition style differs"); }
        if (expected.style == PARTITION_STYLE_GPT && !IsEqualGUID(current->gptDiskId, expected.gptDiskId))
        {
            refuse(L"GPT disk GUID differs");
        }

        if (expected.style == PARTITION_STYLE_MBR && current->mbrSignature != expected.mbrSignature)
        {
            refuse(L"MBR signature differs");
        }
    }

    void SetDiskOffline(HANDLE disk, bool offline)
    {
        SET_DISK_ATTRIBUTES attributes{};
        attributes.Version = sizeof(attributes);
        attributes.Persist = TRUE;
        attributes.Attributes = offline ? DISK_ATTRIBUTE_OFFLINE : 0;
        attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
        DWORD bytesReturned = 0;
        if (!DeviceIoControl(disk, IOCTL_DISK_SET_DISK_ATTRIBUTES, &attributes, sizeof(attributes),
                nullptr, 0, &bytesReturned, nullptr))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure,
                offline ? L"failed to set disk offline" : L"failed to set disk online", lastError);
        }
    }

    void DeleteDriveLayout(HANDLE disk)
    {
        // Fails with ERROR_INVALID_FUNCTION on already-RAW disks; that is fine.
        DWORD bytesReturned = 0;
        DeviceIoControl(disk, IOCTL_DISK_DELETE_DRIVE_LAYOUT, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    }

    void CreateDiskStyle(HANDLE disk, PARTITION_STYLE style, const GUID& gptDiskId, uint32_t mbrSignature)
    {
        CREATE_DISK createDisk{};
        if (style == PARTITION_STYLE_GPT)
        {
            createDisk.PartitionStyle = PARTITION_STYLE_GPT;
            createDisk.Gpt.DiskId = gptDiskId;
            createDisk.Gpt.MaxPartitionCount = 128;
        }
        else
        {
            createDisk.PartitionStyle = PARTITION_STYLE_MBR;
            createDisk.Mbr.Signature = mbrSignature;
        }

        DWORD bytesReturned = 0;
        if (!DeviceIoControl(disk, IOCTL_DISK_CREATE_DISK, &createDisk, sizeof(createDisk), nullptr, 0, &bytesReturned, nullptr))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"IOCTL_DISK_CREATE_DISK failed", lastError);
        }
    }

    void SetDriveLayout(HANDLE disk, const std::vector<uint8_t>& layoutBuffer)
    {
        DWORD bytesReturned = 0;
        if (!DeviceIoControl(disk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
                const_cast<uint8_t*>(layoutBuffer.data()),
                static_cast<DWORD>(layoutBuffer.size()), nullptr, 0, &bytesReturned, nullptr))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed", lastError);
        }
    }

    void UpdateDiskProperties(HANDLE disk)
    {
        DWORD bytesReturned = 0;
        DeviceIoControl(disk, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    }
}
