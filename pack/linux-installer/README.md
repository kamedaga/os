# Linux Live installer for PachaOS

SystemRescue 13.02 runs the installer. The target NVMe receives
PachaOS, not Linux. Its existing curated ROOTFS.TAR contains Java 25, Prism,
Minecraft/Fabric, Xfce, Epiphany and SSH configuration.

Download the official `systemrescue-13.02-amd64.iso` into
`.artifacts/linux-minecraft-installer/`, then build:

```sh
python3 pack/scripts/build_linux_minecraft_installer.py \
  --ssh-authorized-key-file /path/to/public-key.pub
```

The builder checks the pinned official ISO SHA256, validates the BootFS root
UUID with the existing parser, adds configuration and payload files through
xorriso, and preserves BIOS/UEFI hybrid boot metadata. It does not unpack or
modify the Linux root filesystem or rebuild the developer rootfs. Output:
`.artifacts/linux-minecraft-installer/pachaos-linux-installer.iso`.

The default coexistence mode preserves Windows and every existing partition. Before booting
the USB, use Windows Disk Management to create unallocated space; free space
inside C: is not sufficient. The installer never shrinks, moves, deletes, or
reformats an existing partition. An existing GPT, EFI partition and Microsoft
data partition are required. Keep the Windows recovery key available if device
encryption is enabled, since firmware/boot changes can require recovery.

Write the image to the intended USB with verification. Boot it in UEFI mode,
The default `Linux maintenance (USB recovery)` entry uses the old USB enumeration
scheme, a 10-second initial descriptor timeout, disabled USB autosuspend and
the local console. The original installer entry remains selectable. These
options require real-hardware validation for USB detection reliability.
wait for the Linux shell, then preview with `pacha-install --plan`. Identify the
NVMe by model, serial and size. At least 16.5 GiB of contiguous unallocated
space is required: 512 MiB for a new PachaOS ESP and at least 16 GiB for ext4.
By default the largest eligible gap is selected and root is capped at 64 GiB;
`--root-size-gib N` requests a specific size. The plan lists exact additions.
Run `pacha-install` and enter `ADD PACHAOS /dev/nvmeXnY` only after checking
that plan. Mounted or otherwise occupied targets are rejected. No target is
chosen automatically. To replace Windows and every partition on the selected
NVMe, explicitly use `pacha-install --erase-disk --plan /dev/nvmeXnY`, then
`pacha-install --erase-disk /dev/nvmeXnY`. The write requires the exact phrase
`ERASE DISK /dev/nvmeXnY`; the coexistence phrase is rejected. This recreates GPT,
formats a 512 MiB EFI partition and uses the remaining disk for root by default.
`--root-size-gib N` can limit root to a requested size. Blank, MBR and GPT disks
are supported. Erase mode puts PachaOS first in UEFI BootOrder, retaining old
firmware entries. It does not securely sanitize the former contents of the disk.

In coexistence mode, only the two new partitions are formatted. The original GPT entries are
compared before and after append, before either new device is formatted. GPT
backups and the plan remain in `/run/pacha-install/`. The root archive is
extracted directly from USB to NVMe; both new filesystems are then unmounted.
PachaOS has its own ESP, so Microsoft's EFI files and fallback loader are never
mounted or changed. Its UEFI entry is appended to the existing BootOrder,
preserving the previous default and the relative order of all old entries.
The UUID comes from SYSTEM.BOOTFS; ext4 orphan_file is disabled to match the
current PachaOS shutdown behavior. It neither stages the archive in RAM nor
rehashes the entire payload on each boot/install.

SSH is available from the Linux Live system using only the supplied public
key. Its host keys are generated in RAM at boot. The startup prints its IP
addresses. Run the installer locally or over SSH; failures leave the Linux
shell and SSH available. Log: `/run/pacha-install/install.log`.

After success, run `poweroff`, remove the USB and select PachaOS in the firmware
boot menu. Windows remains the default if it was the previous default. Start the
installed desktop with `pacha-desktop`. Physical RX 9060 XT rendering and sound
remain hardware validation tasks.

Partition preservation tests: `python3 tests/test_linux_installer_coexist.py`.
They cover 512-byte/4-KiB planning, insufficient or invalid space, changed GPT
entries, UEFI order preservation and a real sfdisk append to a disposable file.
They also cover whole-disk GPT replacement of blank, MBR and GPT images.
Run `tests/run_linux_installer_coexist_qemu.py --iso IMAGE --ssh-key KEY`
for a disposable NVMe installation/boot/SSH test; add `--erase-disk` to test
replacement instead of coexistence. These tests never select physical disks.

To update only the installer on an existing ISO, use
`pack/scripts/update_linux_installer.py --image IMAGE --expect-sha256 OLD_SHA --output-dir DIR`.
It grows the ISO in place and emits `usb-patch.json` and `usb-patch.bin` for
only changed 2048-byte sectors. It verifies that ROOTFS.TAR's extent and data
remain unchanged and that no patch range overlaps it. A USB patch applicator
must verify the old complete image hash before writing and the new complete
image hash after writing. Test the patch generator with
`python3 tests/test_linux_installer_update.py`.

Upstream documentation: [download](https://www.system-rescue.org/Download/),
[configuration](https://www.system-rescue.org/manual/Configuring_SystemRescue/),
[autorun](https://www.system-rescue.org/manual/Run_your_own_scripts_with_autorun/).
