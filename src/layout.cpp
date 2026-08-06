#include "layout.h"

#include <winioctl.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>

namespace DiskClone
{
    namespace
    {
        const DiskInfo* FindDisk(const std::vector<DiskInfo>& disks, int number)
        {
            for (const auto& disk : disks)
            {
                if (disk.number == number) { return &disk; }
            }

            return nullptr;
        }

        [[noreturn]] void Refuse(const std::wstring& why)
        {
            throw Error(ExitCode::SafetyRefusal, why);
        }

        GUID NewGuid()
        {
            GUID guid{};
            // Runs during planning, before anything is written: refusal, not copy failure.
            if (FAILED(CoCreateGuid(&guid)))
            {
                throw Error(ExitCode::SafetyRefusal, L"CoCreateGuid failed");
            }

            return guid;
        }

        // Identify the partition holding the running OS volume, or fall back
        // to the largest NTFS basic-data partition.
        size_t FindWindowsPartition(const DiskInfo& sourceDisk,
                                    const std::vector<PlannedPartition>& parts,
                                    const std::vector<VolumeInfo>& volumes,
                                    bool sourceIsSystemDisk)
        {
            if (sourceIsSystemDisk)
            {
                std::wstring systemVolume = SystemVolumeGuidPath();
                for (const auto& volume : volumes)
                {
                    if (volume.guidPath != systemVolume || volume.diskNumber != sourceDisk.number) { continue; }
                    for (size_t i = 0; i < parts.size(); ++i)
                    {
                        if (parts[i].src.offset == volume.offset) { return i; }
                    }
                }
            }

            // Largest NTFS basic-data partition.
            size_t bestIndex = SIZE_MAX;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                const auto& partition = parts[i];
                bool basicData = sourceDisk.style == PARTITION_STYLE_GPT
                    ? IsEqualGUID(partition.src.gptType, kBasicDataType)
                    : (partition.src.mbrType == 0x07);
                if (partition.fs == FsKind::Ntfs && basicData &&
                    (bestIndex == SIZE_MAX || partition.src.length > parts[bestIndex].src.length))
                {
                    bestIndex = i;
                }
            }

            return bestIndex;
        }

        std::wstring PartitionKindName(const ClonePlan& plan, const PlannedPartition& partition)
        {
            if (plan.sourceDisk.style == PARTITION_STYLE_GPT)
            {
                if (IsEqualGUID(partition.src.gptType, kEspType)) { return L"ESP"; }
                if (IsEqualGUID(partition.src.gptType, kMsrType)) { return L"MSR"; }
                if (IsEqualGUID(partition.src.gptType, kWinReType)) { return L"Recovery"; }
                if (IsEqualGUID(partition.src.gptType, kBasicDataType)) { return L"Data"; }
                return L"Other";
            }

            switch (partition.src.mbrType)
            {
            case 0x07: return L"NTFS";
            case 0x0B:
            case 0x0C: return L"FAT32";
            case 0x27: return L"Recovery";
            default:
            {
                wchar_t label[16];
                swprintf_s(label, L"0x%02X", partition.src.mbrType);
                return label;
            }
            }
        }
    }

    ClonePlan BuildClonePlan(const CloneOptions& opts,
                             const std::vector<DiskInfo>& disks,
                             const std::vector<VolumeInfo>& volumes)
    {
        const DiskInfo* sourceDisk = FindDisk(disks, opts.source);
        const DiskInfo* targetDisk = FindDisk(disks, opts.target);
        if (!sourceDisk) { Refuse(L"source disk " + std::to_wstring(opts.source) + L" not found"); }
        if (!targetDisk) { Refuse(L"target disk " + std::to_wstring(opts.target) + L" not found"); }
        if (opts.source == opts.target) { Refuse(L"source and target are the same disk"); }
        if (targetDisk->isSystemDisk) { Refuse(L"target disk hosts the running operating system"); }
        if (targetDisk->isBootDisk) { Refuse(L"target disk hosts the system partition (boot files) in use by this machine"); }
        if (targetDisk->hasPagefile) { Refuse(L"target disk hosts an active pagefile"); }
        if (!sourceDisk->layoutKnown) { Refuse(L"cannot read the source disk's partition table (I/O error); refusing to guess"); }
        if (!targetDisk->layoutKnown)
        {
            Refuse(L"cannot read the target disk's partition table (I/O error); refusing to treat it as empty — the dynamic-disk check needs it");
        }

        if (IsDynamicOrStorageSpaces(*sourceDisk)) { Refuse(L"source is a dynamic disk or Storage Spaces member (unsupported)"); }
        if (IsDynamicOrStorageSpaces(*targetDisk)) { Refuse(L"target is a dynamic disk or Storage Spaces member (unsupported)"); }
        if (sourceDisk->logicalSectorSize != targetDisk->logicalSectorSize)
        {
            Refuse(L"logical sector sizes differ (source " + std::to_wstring(sourceDisk->logicalSectorSize) +
                L", target " + std::to_wstring(targetDisk->logicalSectorSize) +
                L") — cloning across 512e/4Kn is unsupported");
        }

        if (sourceDisk->style != PARTITION_STYLE_GPT && sourceDisk->style != PARTITION_STYLE_MBR)
        {
            Refuse(L"source disk has no recognized partition table (RAW)");
        }

        if (sourceDisk->partitions.empty()) { Refuse(L"source disk has no partitions"); }

        if (sourceDisk->style == PARTITION_STYLE_MBR)
        {
            for (const auto& partition : sourceDisk->partitions)
            {
                if (partition.mbrType == 0x05 || partition.mbrType == 0x0F)
                {
                    Refuse(L"source has MBR extended/logical partitions (unsupported in v1)");
                }
            }
        }

        ClonePlan plan;
        plan.opts = opts;
        plan.sourceDisk = *sourceDisk;
        plan.targetDisk = *targetDisk;
        plan.sourceIsLive = sourceDisk->isSystemDisk;

        // Sniff filesystems through a read handle on the source disk. BitLocker
        // (on OR suspended) is refused here: volsnap sits above fvevol, so a
        // shadow-copy read returns plaintext, and copying plaintext under FVE
        // metadata would produce an incoherent clone.
        wil::unique_hfile sourceDiskHandle = OpenPhysicalDisk(sourceDisk->number, GENERIC_READ);
        for (const auto& partition : sourceDisk->partitions)
        {
            PlannedPartition planned;
            planned.src = partition;
            planned.targetOffset = partition.offset;
            planned.targetLength = partition.length;

            bool isMsr = sourceDisk->style == PARTITION_STYLE_GPT && IsEqualGUID(partition.gptType, kMsrType);
            planned.fs = isMsr
                ? FsKind::Unknown
                : SniffFilesystem(sourceDiskHandle.get(), partition.offset, sourceDisk->logicalSectorSize);
            if (planned.fs == FsKind::BitLocker)
            {
                Refuse(L"partition at offset " + FormatBytes(partition.offset) +
                    L" is BitLocker-protected (on or suspended). Fully decrypt first: manage-bde -off <letter>:");
            }

            planned.strategy = planned.fs == FsKind::Ntfs ? CopyStrategy::NtfsBitmap : CopyStrategy::Raw;

            for (const auto& volume : volumes)
            {
                if (volume.diskNumber == sourceDisk->number && volume.offset == partition.offset)
                {
                    planned.srcVolumeGuidPath = volume.guidPath;
                }
            }

            plan.parts.push_back(std::move(planned));
        }

        // Required size without expansion: end of last partition + GPT reserve.
        uint64_t gptReserve = sourceDisk->style == PARTITION_STYLE_GPT ? kMiB : 0;
        uint64_t lastPartitionEnd = 0;
        for (const auto& planned : plan.parts)
        {
            lastPartitionEnd = std::max(lastPartitionEnd, planned.src.offset + planned.src.length);
        }

        plan.requiredTargetBytes = lastPartitionEnd + gptReserve;
        if (targetDisk->size < plan.requiredTargetBytes)
        {
            Refuse(L"target too small: " + FormatBytes(targetDisk->size) + L" < required " +
                FormatBytes(plan.requiredTargetBytes) + L" (shrinking is unsupported in v1)");
        }

        // Identify the Windows partition ONCE, here — --expand sizing and the
        // --new-ids finalizer (bcdboot target) must agree on the same partition.
        {
            size_t windowsIndex = FindWindowsPartition(*sourceDisk, plan.parts, volumes, sourceDisk->isSystemDisk);
            if (windowsIndex != SIZE_MAX)
            {
                plan.parts[windowsIndex].isWindowsPartition = true;
            }
            else if (opts.expand)
            {
                Refuse(L"--expand: cannot identify the Windows partition (no NTFS basic-data partition); retry without --expand");
            }
            else if (opts.newIds)
            {
                Refuse(L"--new-ids: cannot identify the Windows partition (no NTFS basic-data partition) for bcdboot");
            }
        }

        // --expand: relocate trailing partitions to disk end, grow Windows partition.
        if (opts.expand)
        {
            size_t windowsIndex = 0;
            while (windowsIndex < plan.parts.size() && !plan.parts[windowsIndex].isWindowsPartition)
            {
                ++windowsIndex;
            }

            uint64_t usableEnd = AlignDown(targetDisk->size - gptReserve, kMiB);
            if (sourceDisk->style == PARTITION_STYLE_MBR)
            {
                constexpr uint64_t k2TiB = 2ull * 1024 * 1024 * 1024 * 1024;
                if (usableEnd > k2TiB)
                {
                    fwprintf(stderr, L"warning: MBR addressing caps usable space at 2 TiB; capping expansion\n");
                    usableEnd = k2TiB;
                }
            }

            // Trailing partitions (start after the Windows partition start):
            // pack at the disk end in reverse order. AlignDown keeps every
            // start MiB-aligned and non-overlapping even for partitions whose
            // sizes are not MiB multiples.
            uint64_t cursor = usableEnd;
            uint64_t windowsStart = plan.parts[windowsIndex].src.offset;
            for (size_t i = plan.parts.size(); i-- > 0;)
            {
                auto& planned = plan.parts[i];
                if (planned.src.offset <= windowsStart || planned.isWindowsPartition) { continue; }
                uint64_t relocatedStart = AlignDown(cursor - planned.src.length, kMiB);
                planned.targetOffset = relocatedStart;
                planned.targetLength = planned.src.length;
                cursor = relocatedStart;
            }

            auto& windowsPartition = plan.parts[windowsIndex];
            if (cursor < windowsPartition.src.offset + windowsPartition.src.length)
            {
                Refuse(L"--expand produced a smaller Windows partition; target layout does not fit");
            }

            windowsPartition.targetLength = cursor - windowsPartition.src.offset;
        }

        // Copy-volume upper bound for progress totals.
        for (const auto& planned : plan.parts)
        {
            plan.totalCopyBytes += planned.src.length;
        }

        // Identity: default mode preserves everything (true clone, swap-and-boot);
        // --new-ids regenerates the disk identity AND the partition unique GUIDs
        // (type GUIDs, attributes, and names are preserved) so the clone can
        // coexist online next to the source.
        if (opts.newIds)
        {
            plan.targetGptDiskId = NewGuid();
            uint32_t signature = static_cast<uint32_t>(GetTickCount64() ^ (GetCurrentProcessId() << 16));
            plan.targetMbrSignature = signature ? signature : 0x1234ABCD;
            for (auto& planned : plan.parts)
            {
                planned.src.gptId = NewGuid();
            }
        }
        else
        {
            plan.targetGptDiskId = sourceDisk->gptDiskId;
            plan.targetMbrSignature = sourceDisk->mbrSignature;
        }

        return plan;
    }

    std::vector<uint8_t> BuildTargetLayoutBuffer(const ClonePlan& plan)
    {
        size_t partitionCount = plan.parts.size();

        // MBR layouts must carry entries in multiples of 4 (the primary
        // table's slots), with unused slots present as PARTITION_ENTRY_UNUSED —
        // IOCTL_DISK_SET_DRIVE_LAYOUT_EX rejects other counts.
        size_t emittedCount = plan.sourceDisk.style == PARTITION_STYLE_MBR
            ? ((partitionCount + 3) / 4) * 4
            : partitionCount;
        size_t bufferBytes = offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
                             emittedCount * sizeof(PARTITION_INFORMATION_EX);
        std::vector<uint8_t> layoutBuffer(bufferBytes, 0);
        auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuffer.data());
        layout->PartitionCount = static_cast<DWORD>(emittedCount);

        if (plan.sourceDisk.style == PARTITION_STYLE_GPT)
        {
            layout->PartitionStyle = PARTITION_STYLE_GPT;
            layout->Gpt.DiskId = plan.targetGptDiskId;
            layout->Gpt.StartingUsableOffset.QuadPart = 0;   // let the driver compute
            layout->Gpt.UsableLength.QuadPart = 0;
            layout->Gpt.MaxPartitionCount = 128;
        }
        else
        {
            layout->PartitionStyle = PARTITION_STYLE_MBR;
            layout->Mbr.Signature = plan.targetMbrSignature;
        }

        for (size_t i = 0; i < partitionCount; ++i)
        {
            const auto& planned = plan.parts[i];
            PARTITION_INFORMATION_EX& entry = layout->PartitionEntry[i];
            entry.StartingOffset.QuadPart = static_cast<LONGLONG>(planned.targetOffset);
            entry.PartitionLength.QuadPart = static_cast<LONGLONG>(planned.targetLength);
            entry.PartitionNumber = static_cast<DWORD>(i + 1);
            entry.RewritePartition = TRUE;
            if (plan.sourceDisk.style == PARTITION_STYLE_GPT)
            {
                entry.PartitionStyle = PARTITION_STYLE_GPT;
                entry.Gpt.PartitionType = planned.src.gptType;
                entry.Gpt.PartitionId = planned.src.gptId;
                entry.Gpt.Attributes = planned.src.gptAttributes;
                wcsncpy_s(entry.Gpt.Name, planned.src.gptName.c_str(), _TRUNCATE);
            }
            else
            {
                entry.PartitionStyle = PARTITION_STYLE_MBR;
                entry.Mbr.PartitionType = planned.src.mbrType;
                entry.Mbr.BootIndicator = planned.src.mbrActive ? TRUE : FALSE;
                entry.Mbr.RecognizedPartition = TRUE;
                entry.Mbr.HiddenSectors = static_cast<DWORD>(planned.targetOffset / plan.sourceDisk.logicalSectorSize);
            }
        }

        // Pad MBR layouts to the emitted count with explicit unused entries.
        for (size_t i = partitionCount; i < emittedCount; ++i)
        {
            PARTITION_INFORMATION_EX& entry = layout->PartitionEntry[i];
            entry.PartitionStyle = PARTITION_STYLE_MBR;
            entry.PartitionNumber = static_cast<DWORD>(i + 1);
            entry.RewritePartition = TRUE;
            entry.Mbr.PartitionType = PARTITION_ENTRY_UNUSED;
        }

        return layoutBuffer;
    }

    void PrintPlan(const ClonePlan& plan)
    {
        wprintf(L"\nClone plan: disk %d -> disk %d%s%s\n",
            plan.opts.source, plan.opts.target,
            plan.opts.expand ? L"  [--expand]" : L"",
            plan.opts.newIds ? L"  [--new-ids]" : L"");
        wprintf(L"  Source: %s, %s, %s%s\n",
            plan.sourceDisk.model.c_str(), FormatBytes(plan.sourceDisk.size).c_str(),
            plan.sourceDisk.style == PARTITION_STYLE_GPT ? L"GPT" : L"MBR",
            plan.sourceIsLive ? L" (RUNNING SYSTEM — VSS snapshot will be used)" : L"");
        wprintf(L"  Target: %s, %s  ** ALL DATA WILL BE DESTROYED **\n",
            plan.targetDisk.model.c_str(), FormatBytes(plan.targetDisk.size).c_str());
        wprintf(L"  Identity: %s\n\n",
            plan.opts.newIds ? L"new disk/partition IDs + bcdboot repair"
                             : L"preserved (swap disks before first boot from clone)");
        wprintf(L"  %-4s %-9s %-13s %-13s %-13s %-13s %-7s %s\n",
            L"#", L"Kind", L"Src offset", L"Src size", L"Tgt offset", L"Tgt size", L"FS", L"Copy");
        for (size_t i = 0; i < plan.parts.size(); ++i)
        {
            const auto& planned = plan.parts[i];
            wprintf(L"  %-4zu %-9s %-13s %-13s %-13s %-13s %-7s %s%s\n",
                i + 1, PartitionKindName(plan, planned).c_str(),
                FormatBytes(planned.src.offset).c_str(), FormatBytes(planned.src.length).c_str(),
                FormatBytes(planned.targetOffset).c_str(), FormatBytes(planned.targetLength).c_str(),
                FsKindName(planned.fs),
                planned.strategy == CopyStrategy::NtfsBitmap ? L"used clusters" : L"raw",
                !planned.isWindowsPartition ? L""
                    : plan.opts.expand ? L" (Windows, expanded)" : L" (Windows)");
        }

        wprintf(L"\n");
    }
}
