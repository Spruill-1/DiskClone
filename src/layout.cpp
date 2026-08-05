#include "layout.h"

#include <winioctl.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>

namespace dc {

static const DiskInfo* FindDisk(const std::vector<DiskInfo>& disks, int number) {
    for (const auto& d : disks)
        if (d.number == number) return &d;
    return nullptr;
}

[[noreturn]] static void Refuse(const std::wstring& why) {
    throw Error(ExitCode::SafetyRefusal, why);
}

static GUID NewGuid() {
    GUID g{};
    // Runs during planning, before anything is written: refusal, not copy failure.
    if (FAILED(CoCreateGuid(&g)))
        throw Error(ExitCode::SafetyRefusal, L"CoCreateGuid failed");
    return g;
}

// Identify the partition holding the running OS volume, or fall back to the
// largest NTFS basic-data partition.
static size_t FindWindowsPartition(const DiskInfo& src,
                                   const std::vector<PlannedPartition>& parts,
                                   const std::vector<VolumeInfo>& volumes,
                                   bool sourceIsSystemDisk) {
    if (sourceIsSystemDisk) {
        std::wstring sysVol = SystemVolumeGuidPath();
        for (const auto& v : volumes) {
            if (v.guidPath != sysVol || v.diskNumber != src.number) continue;
            for (size_t i = 0; i < parts.size(); ++i)
                if (parts[i].src.offset == v.offset)
                    return i;
        }
    }
    // Largest NTFS basic-data partition.
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& p = parts[i];
        bool basicData = src.style == PARTITION_STYLE_GPT
            ? IsEqualGUID(p.src.gptType, kBasicDataType)
            : (p.src.mbrType == 0x07);
        if (p.fs == FsKind::Ntfs && basicData &&
            (best == SIZE_MAX || p.src.length > parts[best].src.length))
            best = i;
    }
    return best;
}

ClonePlan BuildClonePlan(const CloneOptions& opts,
                         const std::vector<DiskInfo>& disks,
                         const std::vector<VolumeInfo>& volumes) {
    const DiskInfo* src = FindDisk(disks, opts.source);
    const DiskInfo* tgt = FindDisk(disks, opts.target);
    if (!src) Refuse(L"source disk " + std::to_wstring(opts.source) + L" not found");
    if (!tgt) Refuse(L"target disk " + std::to_wstring(opts.target) + L" not found");
    if (opts.source == opts.target) Refuse(L"source and target are the same disk");
    if (tgt->isSystemDisk) Refuse(L"target disk hosts the running operating system");
    if (tgt->isBootDisk) Refuse(L"target disk hosts the system partition (boot files) in use by this machine");
    if (tgt->hasPagefile) Refuse(L"target disk hosts an active pagefile");
    if (!src->layoutKnown) Refuse(L"cannot read the source disk's partition table (I/O error); refusing to guess");
    if (!tgt->layoutKnown)
        Refuse(L"cannot read the target disk's partition table (I/O error); refusing to treat it as empty — the dynamic-disk check needs it");
    if (IsDynamicOrStorageSpaces(*src)) Refuse(L"source is a dynamic disk or Storage Spaces member (unsupported)");
    if (IsDynamicOrStorageSpaces(*tgt)) Refuse(L"target is a dynamic disk or Storage Spaces member (unsupported)");
    if (src->logicalSectorSize != tgt->logicalSectorSize)
        Refuse(L"logical sector sizes differ (source " + std::to_wstring(src->logicalSectorSize) +
               L", target " + std::to_wstring(tgt->logicalSectorSize) +
               L") — cloning across 512e/4Kn is unsupported");
    if (src->style != PARTITION_STYLE_GPT && src->style != PARTITION_STYLE_MBR)
        Refuse(L"source disk has no recognized partition table (RAW)");
    if (src->partitions.empty())
        Refuse(L"source disk has no partitions");

    if (src->style == PARTITION_STYLE_MBR) {
        for (const auto& p : src->partitions) {
            if (p.mbrType == 0x05 || p.mbrType == 0x0F)
                Refuse(L"source has MBR extended/logical partitions (unsupported in v1)");
        }
    }

    ClonePlan plan;
    plan.opts = opts;
    plan.sourceDisk = *src;
    plan.targetDisk = *tgt;
    plan.sourceIsLive = src->isSystemDisk;

    // Sniff filesystems through a read handle on the source disk.
    unique_handle sh = OpenPhysicalDisk(src->number, GENERIC_READ);
    for (const auto& p : src->partitions) {
        PlannedPartition pp;
        pp.src = p;
        pp.targetOffset = p.offset;
        pp.targetLength = p.length;
        bool isMsr = src->style == PARTITION_STYLE_GPT && IsEqualGUID(p.gptType, kMsrType);
        pp.fs = isMsr ? FsKind::Unknown : SniffFilesystem(sh.get(), p.offset, src->logicalSectorSize);
        if (pp.fs == FsKind::BitLocker)
            Refuse(L"partition at offset " + FormatBytes(p.offset) +
                   L" is BitLocker-protected (on or suspended). Fully decrypt first: manage-bde -off <letter>:");
        pp.strategy = pp.fs == FsKind::Ntfs ? CopyStrategy::NtfsBitmap : CopyStrategy::Raw;
        for (const auto& v : volumes)
            if (v.diskNumber == src->number && v.offset == p.offset)
                pp.srcVolumeGuidPath = v.guidPath;
        plan.parts.push_back(std::move(pp));
    }

    // Required size without expansion: end of last partition + GPT reserve.
    uint64_t gptReserve = src->style == PARTITION_STYLE_GPT ? kMiB : 0;
    uint64_t lastEnd = 0;
    for (const auto& p : plan.parts)
        lastEnd = std::max(lastEnd, p.src.offset + p.src.length);
    plan.requiredTargetBytes = lastEnd + gptReserve;
    if (tgt->size < plan.requiredTargetBytes)
        Refuse(L"target too small: " + FormatBytes(tgt->size) + L" < required " +
               FormatBytes(plan.requiredTargetBytes) + L" (shrinking is unsupported in v1)");

    // Identify the Windows partition ONCE, here — --expand sizing and the
    // --new-ids finalizer (bcdboot target) must agree on the same partition.
    {
        size_t wIdx = FindWindowsPartition(*src, plan.parts, volumes, src->isSystemDisk);
        if (wIdx != SIZE_MAX)
            plan.parts[wIdx].isWindowsPartition = true;
        else if (opts.expand)
            Refuse(L"--expand: cannot identify the Windows partition (no NTFS basic-data partition); retry without --expand");
        else if (opts.newIds)
            Refuse(L"--new-ids: cannot identify the Windows partition (no NTFS basic-data partition) for bcdboot");
    }

    // --expand: relocate trailing partitions to disk end, grow Windows partition.
    if (opts.expand) {
        size_t wIdx = 0;
        while (wIdx < plan.parts.size() && !plan.parts[wIdx].isWindowsPartition) ++wIdx;

        uint64_t usableEnd = AlignDown(tgt->size - gptReserve, kMiB);
        if (src->style == PARTITION_STYLE_MBR) {
            constexpr uint64_t k2TiB = 2ull * 1024 * 1024 * 1024 * 1024;
            if (usableEnd > k2TiB) {
                fwprintf(stderr, L"warning: MBR addressing caps usable space at 2 TiB; capping expansion\n");
                usableEnd = k2TiB;
            }
        }

        // Trailing partitions (start after Windows partition start): pack at end in reverse.
        uint64_t cursor = usableEnd;
        uint64_t wStart = plan.parts[wIdx].src.offset;
        for (size_t i = plan.parts.size(); i-- > 0;) {
            auto& p = plan.parts[i];
            if (p.src.offset <= wStart || p.isWindowsPartition) continue;
            uint64_t start = AlignDown(cursor - p.src.length, kMiB);
            p.targetOffset = start;
            p.targetLength = p.src.length;
            cursor = start;
        }
        auto& w = plan.parts[wIdx];
        if (cursor < w.src.offset + w.src.length)
            Refuse(L"--expand produced a smaller Windows partition; target layout does not fit");
        w.targetLength = cursor - w.src.offset;
    }

    // Copy volume upper bound for progress totals.
    for (const auto& p : plan.parts)
        plan.totalCopyBytes += p.src.length;

    // Identity
    if (opts.newIds) {
        plan.targetGptDiskId = NewGuid();
        uint32_t sig = static_cast<uint32_t>(GetTickCount64() ^ (GetCurrentProcessId() << 16));
        plan.targetMbrSignature = sig ? sig : 0x1234ABCD;
        for (auto& p : plan.parts)
            p.src.gptId = NewGuid();   // fresh unique partition GUIDs (type GUIDs preserved)
    } else {
        plan.targetGptDiskId = src->gptDiskId;
        plan.targetMbrSignature = src->mbrSignature;
    }

    return plan;
}

std::vector<uint8_t> BuildTargetLayoutBuffer(const ClonePlan& plan) {
    size_t count = plan.parts.size();
    // MBR layouts must carry entries in multiples of 4 (the primary table's
    // slots), with unused slots present as PARTITION_ENTRY_UNUSED —
    // IOCTL_DISK_SET_DRIVE_LAYOUT_EX rejects other counts.
    size_t emitted = plan.sourceDisk.style == PARTITION_STYLE_MBR
        ? ((count + 3) / 4) * 4
        : count;
    size_t bytes = offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
                   emitted * sizeof(PARTITION_INFORMATION_EX);
    std::vector<uint8_t> buf(bytes, 0);
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
    layout->PartitionCount = static_cast<DWORD>(emitted);

    if (plan.sourceDisk.style == PARTITION_STYLE_GPT) {
        layout->PartitionStyle = PARTITION_STYLE_GPT;
        layout->Gpt.DiskId = plan.targetGptDiskId;
        layout->Gpt.StartingUsableOffset.QuadPart = 0;   // let the driver compute
        layout->Gpt.UsableLength.QuadPart = 0;
        layout->Gpt.MaxPartitionCount = 128;
    } else {
        layout->PartitionStyle = PARTITION_STYLE_MBR;
        layout->Mbr.Signature = plan.targetMbrSignature;
    }

    for (size_t i = 0; i < count; ++i) {
        const auto& p = plan.parts[i];
        PARTITION_INFORMATION_EX& e = layout->PartitionEntry[i];
        e.StartingOffset.QuadPart = static_cast<LONGLONG>(p.targetOffset);
        e.PartitionLength.QuadPart = static_cast<LONGLONG>(p.targetLength);
        e.PartitionNumber = static_cast<DWORD>(i + 1);
        e.RewritePartition = TRUE;
        if (plan.sourceDisk.style == PARTITION_STYLE_GPT) {
            e.PartitionStyle = PARTITION_STYLE_GPT;
            e.Gpt.PartitionType = p.src.gptType;
            e.Gpt.PartitionId = p.src.gptId;
            e.Gpt.Attributes = p.src.gptAttributes;
            wcsncpy_s(e.Gpt.Name, p.src.gptName.c_str(), _TRUNCATE);
        } else {
            e.PartitionStyle = PARTITION_STYLE_MBR;
            e.Mbr.PartitionType = p.src.mbrType;
            e.Mbr.BootIndicator = p.src.mbrActive ? TRUE : FALSE;
            e.Mbr.RecognizedPartition = TRUE;
            e.Mbr.HiddenSectors = static_cast<DWORD>(p.targetOffset / plan.sourceDisk.logicalSectorSize);
        }
    }
    // Pad MBR layouts to the emitted count with explicit unused entries.
    for (size_t i = count; i < emitted; ++i) {
        PARTITION_INFORMATION_EX& e = layout->PartitionEntry[i];
        e.PartitionStyle = PARTITION_STYLE_MBR;
        e.PartitionNumber = static_cast<DWORD>(i + 1);
        e.RewritePartition = TRUE;
        e.Mbr.PartitionType = PARTITION_ENTRY_UNUSED;
    }
    return buf;
}

static std::wstring PartitionKindName(const ClonePlan& plan, const PlannedPartition& p) {
    if (plan.sourceDisk.style == PARTITION_STYLE_GPT) {
        if (IsEqualGUID(p.src.gptType, kEspType)) return L"ESP";
        if (IsEqualGUID(p.src.gptType, kMsrType)) return L"MSR";
        if (IsEqualGUID(p.src.gptType, kWinReType)) return L"Recovery";
        if (IsEqualGUID(p.src.gptType, kBasicDataType)) return L"Data";
        return L"Other";
    }
    switch (p.src.mbrType) {
    case 0x07: return L"NTFS";
    case 0x0B: case 0x0C: return L"FAT32";
    case 0x27: return L"Recovery";
    default: {
        wchar_t b[16]; swprintf_s(b, L"0x%02X", p.src.mbrType); return b;
    }
    }
}

void PrintPlan(const ClonePlan& plan) {
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
    for (size_t i = 0; i < plan.parts.size(); ++i) {
        const auto& p = plan.parts[i];
        wprintf(L"  %-4zu %-9s %-13s %-13s %-13s %-13s %-7s %s%s\n",
            i + 1, PartitionKindName(plan, p).c_str(),
            FormatBytes(p.src.offset).c_str(), FormatBytes(p.src.length).c_str(),
            FormatBytes(p.targetOffset).c_str(), FormatBytes(p.targetLength).c_str(),
            FsKindName(p.fs),
            p.strategy == CopyStrategy::NtfsBitmap ? L"used clusters" : L"raw",
            p.isWindowsPartition ? L" (Windows, expanded)" : L"");
    }
    wprintf(L"\n");
}

} // namespace dc
