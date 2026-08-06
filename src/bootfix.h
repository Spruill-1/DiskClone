#pragma once

// Bootability finalization.
//
//   - SpliceMbrBootstrap keeps BIOS disks bootable: SET_DRIVE_LAYOUT_EX
//     writes the partition table but never the 440-byte bootstrap code, so
//     that code is copied from the source into the target's sector 0 while
//     preserving the freshly written table, disk signature, and 0x55AA.
//   - FinalizeNewIds is the --new-ids epilogue: online the target, temp-mount
//     the clone's Windows volume and boot partition, rebuild the BCD with
//     inbox bcdboot.exe (always with /s — never against the running system's
//     own boot partition), reset the clone's MountedDevices registry map so
//     its fresh partition IDs re-map at first boot, and extend the NTFS
//     filesystem if --expand grew the Windows partition.
//   - PrintSwapInstructions is the default-mode epilogue.

#include "layout.h"

namespace DiskClone
{
    // Call after SET_DRIVE_LAYOUT_EX on MBR targets.
    void SpliceMbrBootstrap(HANDLE sourceDisk, HANDLE targetDisk, uint32_t sectorSize);

    // Throws ExitCode::FinalizeFailure with manual recovery commands on error;
    // on those failures the temp drive letters are intentionally left mounted
    // so the printed commands work as-is.
    void FinalizeNewIds(const ClonePlan& plan, HANDLE targetDisk);

    // targetOffline reports the ACTUAL end state — if the device rejected the
    // offline attribute the instructions must say so, not claim it was offlined.
    void PrintSwapInstructions(const ClonePlan& plan, bool targetOffline);
}
