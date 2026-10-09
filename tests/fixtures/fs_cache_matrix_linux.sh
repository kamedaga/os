#!/bin/sh
set -eu
mkdir -p /pacha
mount -o rw,noatime /dev/nvme0n1p2 /pacha
mkdir -p /pacha/tmp /pacha/proc /pacha/run
mount -t tmpfs tmpfs /pacha/tmp
mount -t tmpfs tmpfs /pacha/run
mount --bind /proc /pacha/proc
cp /artifacts/native.elf /pacha/tmp/cache-matrix.elf
chmod 755 /pacha/tmp/cache-matrix.elf
touch /pacha/run/kobox-cache-matrix-guest
uname -a
diagnostic=0
if [ -f /artifacts/cold-diagnostic ]; then diagnostic=1; fi
FS_CACHE_DIAGNOSTIC="$diagnostic" chroot /pacha /tmp/cache-matrix.elf --guest-cold
umount /pacha/proc
umount /pacha/run
umount /pacha/tmp
sync
umount /pacha
echo FS_CACHE_NATIVE_OK
