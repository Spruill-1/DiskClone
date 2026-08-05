#pragma once

#include "layout.h"

namespace dc {

// Splices the 440-byte MBR bootstrap from the source disk into the target's
// sector 0, preserving the freshly written partition table, disk signature
// and 0x55AA. Call after SET_DRIVE_LAYOUT_EX on MBR targets.
void SpliceMbrBootstrap(HANDLE sourceDisk, HANDLE targetDisk, uint32_t sectorSize);

// --new-ids finalization: bring the target online, wait for its volumes,
// temp-mount the Windows volume and ESP/system partition, run inbox
// bcdboot.exe to rebuild the BCD, extend the NTFS filesystem if the Windows
// partition was expanded, then remove the temp mount points.
// Throws ExitCode::FinalizeFailure with manual recovery commands on error.
void FinalizeNewIds(const ClonePlan& plan, HANDLE targetDisk);

// Default-mode epilogue: prints swap instructions (and the deferred
// extend-filesystem step when --expand grew the Windows partition).
// targetOffline reports the ACTUAL end state — if the device rejected the
// offline attribute the instructions must say so, not claim it was offlined.
void PrintSwapInstructions(const ClonePlan& plan, bool targetOffline);

} // namespace dc
