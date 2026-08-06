#pragma once

// Physical-disk model and disk-level IOCTL wrappers. Everything here operates
// on \\.\PhysicalDriveN handles:
//   - Enumeration and property capture (bus, model, serial, sector sizes,
//     partition style and layout) into the DiskInfo snapshot the planner uses.
//   - The destructive target-prep operations (offline attribute, layout
//     delete/create/set) invoked by the clone orchestration in main.cpp.
//   - Post-confirmation identity revalidation, closing the TOCTOU window
//     between "the user approved disk N" and "we wiped disk N".

#include "util.h"
#include <winioctl.h>
#include <optional>

namespace DiskClone
{
    struct PartitionInfo
    {
        int number{ 0 };                   // PartitionNumber from the layout
        uint64_t offset{ 0 };              // bytes from disk start
        uint64_t length{ 0 };              // bytes

        // GPT
        GUID gptType{};
        GUID gptId{};
        uint64_t gptAttributes{ 0 };
        std::wstring gptName;

        // MBR
        uint8_t mbrType{ 0 };
        bool mbrActive{ false };
        uint32_t mbrHiddenSectors{ 0 };
    };

    struct DiskInfo
    {
        int number{ -1 };                  // \\.\PhysicalDriveN
        uint64_t size{ 0 };                // bytes
        uint32_t logicalSectorSize{ 512 };
        uint32_t physicalSectorSize{ 512 };
        PARTITION_STYLE style{ PARTITION_STYLE_RAW };
        GUID gptDiskId{};
        uint32_t mbrSignature{ 0 };
        std::wstring busType;              // "SATA", "NVMe", "USB", ...
        std::wstring model;                // vendor + product
        std::wstring serial;
        bool isSystemDisk{ false };        // hosts the running OS volume
        bool isBootDisk{ false };          // hosts the in-use system partition (ESP / boot files)
        bool hasPagefile{ false };         // hosts an active pagefile volume
        bool offline{ false };
        bool layoutKnown{ false };         // IOCTL_DISK_GET_DRIVE_LAYOUT_EX succeeded
        std::vector<PartitionInfo> partitions;
    };

    // Opens \\.\PhysicalDriveN. Throws on failure unless optional=true.
    wil::unique_hfile OpenPhysicalDisk(int number, DWORD access, bool optional = false);

    // Enumerates physical disks 0..N with tolerated numbering gaps, fully populated.
    std::vector<DiskInfo> EnumerateDisks();

    // Re-reads layout/geometry for a single disk number.
    std::optional<DiskInfo> QueryDisk(int number);

    // Re-queries the disk and throws ExitCode::SafetyRefusal if its identity
    // (serial, model, size, sector size, partition style, disk id/signature) no
    // longer matches the snapshot the plan was built from. Call immediately
    // before the first destructive operation — disk numbers get reused when
    // devices come and go, and the confirmation prompt can sit for minutes.
    void RevalidateDiskIdentity(const DiskInfo& expected, const wchar_t* role);

    // Disk-level operations on an open disk handle.
    void SetDiskOffline(HANDLE disk, bool offline);            // IOCTL_DISK_SET_DISK_ATTRIBUTES, Persist=TRUE
    void DeleteDriveLayout(HANDLE disk);
    void CreateDiskStyle(HANDLE disk, PARTITION_STYLE style, const GUID& gptDiskId, uint32_t mbrSignature);
    void SetDriveLayout(HANDLE disk, const std::vector<uint8_t>& layoutBuffer);
    void UpdateDiskProperties(HANDLE disk);

    // True if any partition marks the disk dynamic (LDM) or Storage Spaces.
    bool IsDynamicOrStorageSpaces(const DiskInfo& disk);

    // Well-known GPT partition type GUIDs.
    extern const GUID kEspType;            // EFI System Partition
    extern const GUID kMsrType;            // Microsoft Reserved
    extern const GUID kBasicDataType;      // Basic data
    extern const GUID kWinReType;          // Microsoft Recovery (WinRE)
    extern const GUID kLdmMetadataType;
    extern const GUID kLdmDataType;
    extern const GUID kStorageSpacesType;
}
