# Installer implementation status

The target layout is UEFI/GPT, FAT32 ESP and **ext4 root**, including NVMe.
The installer must use existing filesystem tools; GPT parsing alone does not
provide formatting, mounting or installation.

## Verified building blocks

- Guided GPT planning, write/readback and manual ext4 reuse validation have
  host tests in `tests/installer_*_unit.c`.
- A unified catalog retains each USB/NVMe source capability and generation;
  two sandboxes reporting the same local disk sequence remain distinct.
  Manual reuse now checks FAT32 BPB/FSInfo/backup boot data and ext4 both at
  selection time and again immediately before requesting write authority.
- The selected-disk data port has no enumeration or ARM operation. Its unit
  test covers byte-granular reads/writes, range confinement and preservation
  of neighboring sectors for existing formatter tools.
- LPR now has a capability-gated seekable `/dev/pacha/install-target` path,
  backed by that data port. It does not appear in an ordinary live process;
  the corresponding fixed native endpoint must be granted to a formatter.
- SHA-256-pinned Alpine v3.22 `mkfs.fat`, `mke2fs` and mtools are in the
  installer bootfs overlay. `pack/scripts/test_installer_tools.sh` verifies
  their output and readback on a disposable sparse disk, including an ext4
  image populated from an OS-file directory.
- The installer overlay enables the Kobox2 NVMe package without making it a
  normal live boot dependency.
- QEMU with USB mass storage and NVMe simultaneously reaches the shell,
  enumerates the NVMe namespace, reads its partition metadata, and identifies
  the USB `pacha-install-data` GPT marker. The USB is not a target.
- Native stack buffers now use Linux's existing bounce-buffer path. Linux
  task bookkeeping stacks are not the stacks on which hosted tasks execute;
  failing to classify native stacks made NVMe's timestamp admin request retry
  indefinitely after a rejected DMA map. This is fixed in the Kobox task
  adapter and native host implementations, not in the NVMe driver.

The stack callback changes the private Kobox host contract. Core, sandbox and
modules must be rebuilt as a matching package. It adds no PachaOS syscall.

## Reproducible block boot test

After building the block packages and the installer-overlay bootfs, use an
installer Limine image containing a marked data partition:

```sh
bash pack/scripts/test_installer_block_boot.sh \
  path/to/installer-limine.img path/to/BOOTFS.IMG
```

The test copies its inputs into a new `.artifacts/installer-block-boot.*`
directory, creates a disposable NVMe disk, boots QEMU, checks the serial log,
and verifies both disks and source inputs remain unchanged. Evidence is kept
in that directory. It does not enumerate or write host disks. It verifies
read-only discovery, **not installation or installed-system boot**.

## Connected installer and optional live desktop

The live TUI now launches from `seed0root`, obtains the USB/NVMe catalog and
delegates bounded ports to the existing formatter/deployment runner. It
verifies the payload, populates ext4, deploys and reads back the FAT32 ESP.
The formatter test above verifies this runner on disposable host images;
it is not evidence of a new physical-machine test.

When `<Shell>` is selected, runner contract version 2 prepares the optional
`DESKTOP.TGZ` on the marked installer-data partition. This is a distinct
read-only mode: no SSD is armed, target extents are zero and the media port
does not accept writes. Without the optional bundle the ordinary shell starts.
Import failure also leaves the recovery shell available.

Local desktop testing uses the image writer's verified media without repeating
archive and per-file SHA-256 scans at boot. Explicit `--verify` enables these
diagnostic checks. The importer does not retain the archive in tmpfs and invokes
BusyBox's ordinary applet installer by absolute path, so it does not depend on
`/proc/self/exe`. `/run/pacha/live-desktop.log` records failures and
`/run/pacha/live-desktop.ready` exists only after commands and session state are ready.
`pacha-live-desktop` uses the ordinary upstream startx/Xorg/Xfce path after
the separate manual GPU probe. It does not probe hardware or choose a PCI ID,
monitor mode or software fallback. Interactive desktop monitoring has no duration
deadline or normal stop/power-off sequence. SSH/logging delays are retried, not
treated as proof of GPU failure: the sandbox independently owns thermal protection.
Reported GPU safety failures still enter the emergency stop/power-off procedure.

Build the selected package closure and check it without a USB test:

```sh
python3 pack/scripts/build_live_desktop_bundle.py
python3 tests/test_live_desktop_bundle.py
bash tests/test_live_desktop_import.sh
```

The bundle uses the verified AMD Mesa package for overlapping libraries, so
desktop and GPU-smoke extraction order cannot select different builds.
Alpine's original APK checksums remain intact: `apk audit` can report these
documented local Mesa replacements rather than pretending they are pristine
Alpine package files. No complete developer rootfs is copied.
