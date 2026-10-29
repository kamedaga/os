#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$repo_root/.artifacts"
work="$(mktemp -d "$repo_root/.artifacts/installer-tools-test.XXXXXX")"
disk="$work/target.img"
trap 'echo "Installer formatter evidence: $work"' EXIT
truncate -s 2G "$disk"

tools="$repo_root/.artifacts/installer-tools"
export LD_LIBRARY_PATH="$tools/usr/lib"
"$tools/sbin/mkfs.fat" -F 32 -S 512 -n PACHAESP \
    --offset=2048 "$disk" 524288 > "$work/mkfs-fat.log" 2>&1
cc -std=c11 -Wall -Wextra -Werror \
    "$repo_root/tests/installer_fat32_probe_unit.c" \
    "$repo_root/userland/installer/fat32_probe.c" \
    -o "$work/fat32-probe"
"$work/fat32-probe" "$disk" 512 2048 1048576
# Reproduce the firmware rejection on a large parent disk. dosfstools exits
# successfully but its parent-sized clusters violate FAT32's minimum count.
large_disk="$work/large-parent.img"
truncate -s 128G "$large_disk"
"$tools/sbin/mkfs.fat" -F 32 --offset=2048 "$large_disk" 524288 \
    > "$work/large-parent-fat.log" 2>&1
"$work/fat32-probe" "$large_disk" 512 2048 1048576 -22
disk_4k="$work/target-4k.img"
truncate -s 2G "$disk_4k"
"$tools/sbin/mkfs.fat" -F 32 -S 4096 -n PACHAESP \
    --offset=256 "$disk_4k" 524288 > "$work/mkfs-fat-4k.log" 2>&1
"$work/fat32-probe" "$disk_4k" 4096 256 131072
"$tools/usr/bin/mmd" -i "$disk@@$((2048 * 512))" ::/EFI
"$tools/usr/bin/mcopy" -i "$disk@@$((2048 * 512))" \
    "$repo_root/userland/installer/README.md" ::/EFI/INSTALLER.TXT
"$tools/usr/bin/mcopy" -i "$disk@@$((2048 * 512))" \
    ::/EFI/INSTALLER.TXT "$work/esp-readback.txt"
cmp "$repo_root/userland/installer/README.md" "$work/esp-readback.txt"
root_start=$((1050624 * 512))
root_blocks=$(((2 * 1024 * 1024 * 1024 - root_start - 64 * 4096) / 4096))
"$tools/sbin/mke2fs" -F -q -t ext4 -b 4096 \
    -E "offset=$root_start" "$disk" "$root_blocks" \
    > "$work/mke2fs.log" 2>&1

fat_magic="$(dd if="$disk" bs=1 skip=$((2048 * 512 + 510)) count=2 status=none | od -An -tx1 | tr -d ' \n')"
fat_type="$(dd if="$disk" bs=1 skip=$((2048 * 512 + 82)) count=5 status=none)"
ext_magic="$(dd if="$disk" bs=1 skip=$((root_start + 1024 + 0x38)) count=2 status=none | od -An -tx1 | tr -d ' \n')"
test "$fat_magic" = 55aa
test "$fat_type" = FAT32
test "$ext_magic" = 53ef
test "$(dd if="$disk" bs=1 count=512 status=none | od -An -tu1 | tr -cd '1-9')" = ""
mkdir -p "$work/root-files"
cp "$repo_root/userland/installer/README.md" "$work/root-files/INSTALLER.md"
truncate -s 64M "$work/root-files.img"
"$tools/sbin/mke2fs" -F -q -t ext4 -d "$work/root-files" \
    "$work/root-files.img" > "$work/mke2fs-populate.log" 2>&1
env -u LD_LIBRARY_PATH debugfs -R 'cat /INSTALLER.md' "$work/root-files.img" \
    > "$work/readback.md" 2> "$work/debugfs.log"
cmp "$work/root-files/INSTALLER.md" "$work/readback.md"

# Exercise the same unmodified-tool transaction used by the live installer.
media="$work/install-media.img"
runner_target="$work/runner-target.img"
runner_root="$work/runner-root-view.img"
payload_tree="$work/payload-tree"
mkdir -p "$payload_tree/etc"
printf 'installed-through-runner\n' > "$payload_tree/etc/pacha-test"
tar -C "$payload_tree" -cf "$work/ROOTFS.TAR" .
python3 - "$work/SYSTEM.BOOTFS" <<'PY_BOOTFS'
import struct,sys
# Valid ordinary BOOTFS containing one root identity, not a magic-only stub.
name=b'/etc/pacha/root-uuid';data=bytes.fromhex('11112222333344445555666677778888')
offset=(104+48+len(name)+1+15)&~15
image=bytearray(offset+16)
struct.pack_into('<IIQIIQQQQQ',image,0,0x53465442,104,len(image),1,48,104,152,len(name)+1,offset,16)
struct.pack_into('<IHBBQQI',image,104,0,len(name),1,0,offset,16,0o644)
image[152:152+len(name)]=name;image[offset:]=data
open(sys.argv[1],'wb').write(image)
PY_BOOTFS
cat > "$work/progress-wrapper" <<EOF_PROGRESS
#!/bin/sh
if [ "\$1" = --root-uuid ]; then
    exec "$repo_root/.artifacts/installer-tools/sbin/pacha-install-progress" "\$@"
fi
exit 0
EOF_PROGRESS
chmod 755 "$work/progress-wrapper"
printf '/PachaOS installed\n    protocol: limine\n    path: boot():/KERNEL.ELF\n' > "$work/SYSTEM.CONF"
printf 'kernel\n' > "$work/KERNEL.ELF"
printf 'init\n' > "$work/INITAPP.ELF"
printf 'efi\n' > "$work/BOOTX64.EFI"
(cd "$work" && sha256sum ROOTFS.TAR > ROOTFS.SHA && \
    sha256sum SYSTEM.BOOTFS > SYSTEM.SHA)
printf '1 %s %s %s %s %s %s\n' \
    "$(stat -c %s "$work/ROOTFS.TAR")" \
    "$(stat -c %s "$work/SYSTEM.BOOTFS")" \
    "$(stat -c %s "$work/KERNEL.ELF")" \
    "$(stat -c %s "$work/INITAPP.ELF")" \
    "$(stat -c %s "$work/BOOTX64.EFI")" \
    "$(stat -c %s "$work/SYSTEM.CONF")" > "$work/INSTALL.META"
truncate -s 256M "$media"
media_data=$((64 * 1024 * 1024))
mformat -i "$media@@0" -T $((64 * 1024 * 1024 / 512)) -F ::
mformat -i "$media@@$media_data" -T $((192 * 1024 * 1024 / 512)) -F ::
mmd -i "$media@@0" ::/EFI
mmd -i "$media@@0" ::/EFI/BOOT
mmd -i "$media@@0" ::/EFI/PachaOS
mcopy -i "$media@@0" "$work/KERNEL.ELF" ::/KERNEL.ELF
mcopy -i "$media@@0" "$work/INITAPP.ELF" ::/INITAPP.ELF
mcopy -i "$media@@0" "$work/BOOTX64.EFI" ::/EFI/PachaOS/LIMINE.EFI
for item in ROOTFS.TAR ROOTFS.SHA SYSTEM.BOOTFS SYSTEM.SHA SYSTEM.CONF INSTALL.META; do
    mcopy -i "$media@@$media_data" "$work/$item" "::/$item"
done
truncate -s 512M "$runner_target"
truncate -s 512M "$runner_root"
run_runner() {
PATH="$tools/sbin:$tools/usr/bin:$PATH" \
LD_LIBRARY_PATH="$tools/usr/lib" \
PACHA_INSTALL_TARGET="$runner_target" \
PACHA_INSTALL_MEDIA="$media" \
PACHA_INSTALL_ROOT="$runner_root" \
PACHA_INSTALL_STAGE="$work/runner-stage" \
PACHA_INSTALL_TREE="$work/runner-tree" \
PACHA_INSTALL_LOG="$work/installer.log" \
PACHA_INSTALL_PROGRESS="$work/progress-wrapper" \
PACHA_INSTALL_VERIFY="${PACHA_INSTALL_VERIFY:-0}" \
busybox ash "$repo_root/pack/installer-bootfs/pacha-install-runner" \
    "$1" $((1024 * 1024)) $((512 * 1024 * 1024)) $((512 * 1024 * 1024)) \
    0 "$media_data" > "$work/runner.log" 2>&1
}
for runner_flags in 8 3; do run_runner "$runner_flags"; done
# One preparation serves both processes; normal install never hashes ROOTFS.
test "$(grep -c '^installer: phase=copy-SYSTEM.BOOTFS$' "$work/installer.log")" = 1
! grep -q '^installer: phase=verify-rootfs$' "$work/installer.log"
grep -q 'reusing prepared boot payload' "$work/installer.log"
# The live runner must never retain a second rootfs archive in tmpfs.  Large
# production payloads otherwise fail before filesystem creation with ENOMEM.
test ! -e "$work/runner-stage/ROOTFS.TAR"
"$work/fat32-probe" "$runner_target" 512 0 1048576
mcopy -i "$runner_target" ::/BOOTFS.IMG \
    "$work/installed-bootfs"
cmp "$work/SYSTEM.BOOTFS" "$work/installed-bootfs"
mcopy -i "$runner_target" \
    ::/EFI/PachaOS/LIMINE.EFI "$work/installed-limine"
mcopy -i "$runner_target" \
    ::/EFI/PachaOS/INSTALL.PENDING "$work/installed-pending"
cmp "$work/BOOTX64.EFI" "$work/installed-limine"
grep -qxF 'PachaOS fixed-disk boot registration pending' \
    "$work/installed-pending"
env -u LD_LIBRARY_PATH debugfs -R 'cat /etc/pacha-test' "$runner_root" \
    > "$work/installed-pacha-test" 2> "$work/installed-debugfs.log"
cmp "$payload_tree/etc/pacha-test" "$work/installed-pacha-test"
env -u LD_LIBRARY_PATH dumpe2fs -h "$runner_root" 2>/dev/null |
    grep -q 'Filesystem UUID:.*11112222-3333-4444-5555-666677778888'
# A malformed/zero-UUID boot archive must be rejected before format authority.
printf bad > "$work/bad.bootfs"
if "$repo_root/.artifacts/installer-tools/sbin/pacha-install-progress" --root-uuid "$work/bad.bootfs"; then
    echo 'Malformed root identity was accepted' >&2; exit 1
fi
# Explicit diagnostics still detect corruption; a failed preparation must
# revoke its staging receipt so deployment cannot reuse stale boot files.
PACHA_INSTALL_VERIFY=1 run_runner 8
grep -q '^installer: phase=verify-rootfs$' "$work/installer.log"
printf '%064d  ROOTFS.TAR\n' 0 > "$work/bad-rootfs.sha"
mcopy -o -i "$media@@$media_data" "$work/bad-rootfs.sha" ::/ROOTFS.SHA
if PACHA_INSTALL_VERIFY=1 run_runner 8; then
    echo 'Explicit verification accepted a bad archive hash' >&2; exit 1
fi
test ! -e "$work/runner-stage/prepared"
if run_runner 3; then
    echo 'Deployment accepted a missing preparation receipt' >&2; exit 1
fi
echo 'PASS: pinned tools and the bounded live runner created/populated/verified FAT32 and ext4'
