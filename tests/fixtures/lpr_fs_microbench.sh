#!/bin/sh
set -eu

scale="${1:-2}"
trials="${2:-3}"

# /root is on the persistent rootfs (ext4); /tmp is filed's tmpfs mount.
echo "FS_MICROBENCH_BEGIN scale=$scale trials=$trials"
for trial in $(seq 1 "$trials"); do
  echo "FS_MICROBENCH_TRIAL trial=$trial"
  /cmd/lpr_fs_microbench.elf ext4 /root "$scale"
  /cmd/lpr_fs_microbench.elf tmpfs /tmp "$scale"
done
echo FS_MICROBENCH_""DONE
