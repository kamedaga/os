#!/bin/sh
set -eu
mkdir -p /pacha
mount -o ro,noload /dev/nvme0n1p2 /pacha
printf 'NVME_INSPECT_BEGIN\n'
if [ -f /pacha/etc/alpine-release ]; then cat /pacha/etc/alpine-release; fi
cat /pacha/etc/apk/repositories
ls -l /pacha/bin/apk /pacha/sbin/apk /pacha/usr/bin/clang /pacha/cmd/clang_hello.c /pacha/cmd/chibicc_workload.c /pacha/cmd/lpr_fs_microbench.elf || true
sha256sum /pacha/cmd/lpr_fs_microbench.elf /pacha/lib/pacha/lpr-linux-x86_64.so
printf 'NVME_INSTALLED_PACKAGES\n'
awk '/^P:|^V:/' /pacha/lib/apk/db/installed
printf 'NVME_ROOT_MOUNTS\n'
cat /proc/mounts
umount /pacha
printf 'FILED_NVME_INSPECT_OK\n'
