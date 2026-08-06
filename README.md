# diskclone

A minimal native C++ CLI utility for cloning physical disks on Windows while preserving
bootability. Built exclusively on inbox Microsoft APIs — Win32 disk/volume IOCTLs, the
Volume Shadow Copy Service (VSS) requester API, C++/WinRT for COM interop, and inbox
`bcdboot.exe` for boot-configuration repair. The only library dependency is
[WIL](https://github.com/microsoft/wil) (Microsoft's header-only Windows Implementation
Library, vendored as a submodule) — compile-time only, so the shipped binary still
touches nothing but inbox Windows APIs.

The central use case: clone your **running** system boot drive onto a bigger drive
(HDD → SSD, small SSD → big SSD) and boot from the result.

- **Architectures:** x64 (AMD64) and ARM64
- **Buses:** SATA, NVMe, USB, RAID — anything exposed as `\\.\PhysicalDriveN`
- **Partition styles:** GPT/UEFI and MBR/BIOS, both kept bootable
- **Live cloning:** the running OS disk is snapshotted with VSS (copy-type backup,
  writer-consistent) and copied from the shadow device
- **Fast:** NTFS volumes copy only allocated clusters (volume bitmap), not free space

## Usage

Run from an elevated prompt (the manifest enforces this).

```
diskclone list
diskclone clone --source N --target N [--expand] [--new-ids] [--force] [--dry-run]
```

| Option | Effect |
|---|---|
| `--source N` | Source physical disk number (see `diskclone list`) |
| `--target N` | Target physical disk number — **all data on it is destroyed** |
| `--expand` | Grow the Windows partition to fill a larger target; trailing partitions (e.g. WinRE) are relocated to the end of the disk |
| `--new-ids` | Stamp fresh disk/partition IDs and rebuild the BCD with `bcdboot` so source and clone can stay connected side by side |
| `--force` | Skip the interactive confirmation (scripting) |
| `--dry-run` | Print the clone plan and exit without touching anything |

### The two identity modes

**Default (true clone):** the target keeps the source's GPT disk GUID / MBR signature and
partition GUIDs, so the existing BCD boots unchanged. Because Windows cannot keep two
disks with the same identity online, the target is left **offline**. Shut down, swap the
drives, and boot — firmware forces its boot disk online automatically. If you used
`--expand`, run `diskpart` → `select volume C` → `extend filesystem` once after the first
boot from the clone (the partition was already grown; this grows the filesystem into it).

**`--new-ids`:** the clone gets a fresh identity, is brought online immediately,
`bcdboot` rebuilds its boot files, the clone's `MountedDevices` registry map is
reset (so the boot volume reclaims C: at first boot despite the new partition
IDs), and the filesystem extension happens automatically. Both disks remain
usable side by side. If you later boot the clone, WinRE may need re-registration
there: `reagentc /disable && reagentc /enable`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Usage error |
| 2 | Not elevated |
| 3 | Safety refusal (target not modified) |
| 4 | VSS failure — snapshots are taken *before* the target is touched, so the target is not modified |
| 5 | Copy/I-O error — the error message states explicitly whether the target was already wiped |
| 6 | Finalization failure — data copy succeeded; the message contains the manual commands to finish (temp drive letters are kept mounted so those commands work as printed) |
| 130 | Cancelled by user — the message states the target's actual state |

Every failure after the confirmation prompt appends the target's true state to the
error message ("was NOT modified" vs "previous contents are DESTROYED"), including
whether the target was left with the persistent OFFLINE attribute.

## Safety

The tool refuses to run (exit 3, before any write) when:

- source and target are the same disk, or the target hosts the running OS, the
  in-use system partition (boot files/ESP — even on a different disk than
  C:\Windows), or a pagefile
- either disk is a dynamic disk or Storage Spaces member
- either disk's partition table cannot be read (never treated as "empty")
- logical sector sizes differ (512e ↔ 4Kn cloning is unsupported)
- any source partition is BitLocker-protected — on **or suspended**
  (decrypt fully first: `manage-bde -off C:`)
- the source has MBR extended/logical partitions
- the target is smaller than the source layout requires (no shrinking in v1)

Destructive work additionally requires typing the target disk number, unless
`--force`. Both disks' identities (serial, model, size, sector size, partition
style, disk ID) are re-verified after the confirmation prompt, immediately
before the first destructive operation — a disk swapped during the prompt is
detected and refused.

Consistency guarantees: **all** NTFS source volumes are VSS-snapshotted (not
just the system disk — a mounted data disk is equally torn by a live copy).
If a volume cannot be snapshotted, it is exclusively locked for the copy or
the clone fails; a mounted, writable volume is never silently copied.

## Known v1 limitations

- No shrink-to-smaller-disk support (target must fit the source layout).
- BitLocker sources must be decrypted first; the clone is written unencrypted
  (re-enable BitLocker on the clone after first boot if desired).
- ReFS volumes are copied raw (full partition span) rather than used-clusters-only.
- `pagefile.sys`/`swapfile.sys`/`hiberfil.sys` contents are excluded by VSS and arrive
  as zeroed clusters — harmless; Windows recreates them.
- Cloning between disks with different logical sector sizes is refused.

## Installing

Planned distribution is via winget as a portable package:

```
winget install diskclone
```

(Not yet published — until then, build from source or grab a GitHub release.)

## Building

Visual Studio 2022 (v143) with the Windows SDK. No packages — the single header-only
dependency (WIL) comes in as a git submodule:

```
git clone --recurse-submodules https://github.com/Spruill-1/DiskClone.git
msbuild DiskClone.sln /p:Configuration=Release /p:Platform=x64
msbuild DiskClone.sln /p:Configuration=Release /p:Platform=ARM64
```

Static CRT (`/MT`), C++20, `/W4 /WX`, single self-contained `diskclone.exe` per platform.

## Design notes

- The target's final partition table (including `--expand` sizes) is authored in a single
  `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` call; GPT backup structures land at the true end of
  the target disk automatically.
- NTFS copies read the volume bitmap via `FSCTL_GET_VOLUME_BITMAP` and additionally
  replicate the first 1 MiB and the NTFS backup boot sector (the sector past
  `NumberSectors`, which is absent from the bitmap and unreadable through a mounted
  volume handle — it is reconstructed from sector 0, of which it is by definition a copy).
- MBR targets get the source's 440-byte bootstrap spliced into sector 0 while keeping
  the freshly written partition table, signature, and `0x55AA`.
- The target disk is set offline (persist) for the copy, which both permits raw writes
  and prevents automount interference; devices that reject the offline attribute fall
  back to per-volume lock/dismount with held handles.

## License

MIT — see [LICENSE](LICENSE).
