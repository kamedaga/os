#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

log_dir=.artifacts/test-results/ext4-hardlink-smoke
mkdir -p "$log_dir" .artifacts/tmp

if [[ ${SKIP_SYNC:-0} != 1 ]]; then
  rm -f "$repo_root/.artifacts/disk.img"
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs --force
fi

.artifacts/bin/pacgo qemu-test \
  --cpus 4 \
  --timeout 120s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/ext4_hardlink_smoke.sh' \
  --expect 'EXT4_HARDLINK_BEGIN' \
  --expect 'EXT4_HARDLINK_UPDATE=OK' \
  --expect 'EXT4_HARDLINK_ADD=OK' \
  --expect 'EXT4_HARDLINK_INODE=OK' \
  --expect 'EXT4_HARDLINK_DEL=OK'

cp .artifacts/serial-tty-test.log "$log_dir/serial.log"
cp .artifacts/console-tty-test.log "$log_dir/console.log"
if grep -Fq 'Not supported' "$log_dir/serial.log" "$log_dir/console.log"; then
  printf 'ext4 hardlink smoke: unexpected unsupported operation\n' >&2
  exit 1
fi

# qemu-test stops the VM after the final marker, so validate normal ext4
# journal/orphan recovery on a disposable partition copy rather than repairing
# the source disk.
rootfs_spec=$(sfdisk -d .artifacts/disk.img | awk '/name="rootfs"/ { print; exit }')
rootfs_start=$(printf '%s\n' "$rootfs_spec" | sed -n 's/.*start= *\([0-9][0-9]*\),.*/\1/p')
rootfs_size=$(printf '%s\n' "$rootfs_spec" | sed -n 's/.*size= *\([0-9][0-9]*\),.*/\1/p')
if [[ -z $rootfs_start || -z $rootfs_size ]]; then
  printf 'ext4 hardlink smoke: failed to resolve rootfs partition\n' >&2
  exit 1
fi
recovery_image=$(mktemp .artifacts/tmp/ext4-hardlink.XXXXXX.ext4)
trap 'rm -f "$recovery_image"' EXIT
dd if=.artifacts/disk.img of="$recovery_image" bs=512 \
  skip="$rootfs_start" count="$rootfs_size" conv=sparse status=none

set +e
e2fsck -fy "$recovery_image" >"$log_dir/recovery-fsck.log" 2>&1
recovery_status=$?
set -e
if (( recovery_status > 1 )); then
  printf 'ext4 hardlink smoke: recovery failed status=%s\n' "$recovery_status" >&2
  exit 1
fi
structural_pattern='^(Inode |Block bitmap differences|Inode bitmap differences|Directory inode|Entry |Unattached inode|Multiply-claimed|(Free (blocks|inodes)|Directories) count wrong for group)|UNEXPECTED INCONSISTENCY|filesystem still has errors|corrupt'
if rg -i "$structural_pattern" "$log_dir/recovery-fsck.log"; then
  printf 'ext4 hardlink smoke: recovery required structural repair\n' >&2
  exit 1
fi
e2fsck -fn "$recovery_image" >"$log_dir/post-recovery-fsck.log" 2>&1
printf 'EXT4_HARDLINK_HOST_FSCK=OK\n'
