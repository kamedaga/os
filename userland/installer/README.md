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

## Physical desktop payload

`pack/scripts/build_minecraft_installer_rootfs.py` composes an explicit payload
from the official Alpine base, selected locked Xfce/Epiphany packages, the
verified WebKit overlay, Semeru Java 25, glibc/radeonsi, Prism Launcher and the
prepared Minecraft 26.1.2/Fabric artifacts. It does not copy the developer
rootfs, Minecraft accounts, worlds, SSH host keys or host caches. Pass one
Ed25519 public key with `--ssh-authorized-key-file`.

The `installed-base physical-desktop` profile reuses asynchronous USB/network
readiness, public-key SSH and the manual GPU/GOP handoff from the live session.
It initially provides a console. `pacha-desktop` explicitly requests the GPU
probe and starts upstream Xorg/Xfce after readiness; it never probes at boot.
The ordinary virtio desktop profile is unchanged. Real GPU rendering and sound
on an installed physical system still need hardware verification.

The formatter reads the root UUID from the structurally validated `SYSTEM.BOOTFS` using
`pacha-install-progress --root-uuid` and supplies it to `mke2fs -U`. A randomly
formatted UUID cannot satisfy storage_boot's exact filesystem selection.
The formatter transaction test verifies the resulting on-disk UUID as well as
ESP/root content. Malformed identities fail during preflight.

The live root uses the same current FileD/Kobox filesystem service with an
explicit tmpfs root configuration. Its separate closure manifest grants no PCI
resources; the ext4 configuration still requires a device capability and a
nonzero root UUID. Validated BOOTFS files are imported through the filesystem
service before live readiness is published. Native and Linux musl interpreters
retain executable metadata in BOOTFS.

This payload needs at least 8 GiB RAM for installation: the filesystem service
receives up to 4 GiB and tmpfs limits usage to 75% of that budget. The archive
streams from USB for verification/extraction; it is never retained in RAM
alongside its extracted tree. The hosted page metadata reservation covers the
4 GiB RAM budget and rejects insufficient metadata space before boot.

A disposable NVMe file was installed by the native PachaOS live installer,
including ext4 consistency checking and ESP readback. The next USB boot verified
UEFI registration and started the installed NVMe root. Public-key SSH, Java 25,
login-shell application paths and Java's 24-processor recognition passed with
24 KVM vCPUs. The final payload explicitly records `.ssh` and Dropbear directory
permissions as 0700 and authorized_keys as 0600. Desktop startup leaves driver
paths to each runtime so Java's glibc Mesa cannot inherit musl driver paths.
These two payload corrections were checked on the installed test system and in
the final archive; the full installation transaction was not repeated.
Physical GPU rendering and physical audio still require separate hardware work.

Build the curated media without writing any physical disk:

```sh
PACGO_NIX_DEVELOP=1 bash pack/scripts/build_minecraft_installer.sh \
  /path/to/ssh-ed25519-public-key
```

After installation, reboot once with the USB inserted to register the fixed-disk
UEFI entry, then remove the USB. The installed console's `pacha-desktop` command
requests the manual GPU handoff and starts Xfce; `prismlauncher` opens the prepared
Minecraft/Fabric instance. SSH uses the supplied public key; media contain no
private keys, saved account authentication or worlds.

## Installer diagnosis and staging

The media writer hashes and reads back the complete image. Normal installation
therefore prepares the boot payload once before ARM and reuses that RAM staging
after the NVMe owner restart. It does not stream the root archive for repeated
hashing. `PACHA_INSTALL_VERIFY=1` explicitly enables source SHA verification in
the runner's preparation mode. A failed preparation removes its receipt;
deployment without a completed preparation fails before any formatter runs.
Lightweight metadata/root-UUID checks and post-format consistency/readback
checks remain.

Live SSH is polled before/during the installer and while its runner children
execute. SSH receives no installer disk ports. `/run/pacha/installer.log` records
each runner phase, so a stopped operation can be inspected from an established
SSH session. These changes improve diagnosis; they do not establish the cause
of a physical-machine hang. Processing cancellation is not implemented yet;
the UI disables terminal signal characters while active.
