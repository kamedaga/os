#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

log_dir=.artifacts/test-results/ext4-sync-persistence
mkdir -p "$log_dir" .artifacts/tmp

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

# 2 ブート構成: write フェーズで書いて sync、read フェーズ (別ブート) で永続化を検証。
# 本体は rootfs 同梱の /cmd/ext4_w.sh, /cmd/ext4_r.sh (tests/fixtures/)。
# tty へは各ブートで起動 1 行だけ送る (python tty 直注入は不安定なため不使用)。

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/ext4_w.sh' \
  --expect 'EXT4W_FILE=OK' \
  --expect 'EXT4W_DIR=OK' \
  --expect 'EXT4W_DONE'

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/ext4_r.sh' \
  --expect 'EXT4R_FILE=OK' \
  --expect 'EXT4R_DIR=OK' \
  --expect 'EXT4R_CLEAN=OK' \
  --expect 'EXT4R_SYNC=OK' \
  --expect 'EXT4R_DONE'

# qemu-test terminates QEMU from the host after the success marker.  The
# filesystem is therefore a power-loss image even though guest writes were
# synced: ext4 deliberately leaves needs_recovery/orphan_present set for the
# lifetime of a read-write mount.  Validate journal/orphan recovery on a
# disposable partition copy, reject structural repairs, then require a clean
# read-only check.  Never repair the source disk as part of this test.
rootfs_spec=$(sfdisk -d .artifacts/disk.img | awk '/name="rootfs"/ { print; exit }')
rootfs_start=$(printf '%s\n' "$rootfs_spec" | sed -n 's/.*start= *\([0-9][0-9]*\),.*/\1/p')
rootfs_size=$(printf '%s\n' "$rootfs_spec" | sed -n 's/.*size= *\([0-9][0-9]*\),.*/\1/p')
if [[ -z $rootfs_start || -z $rootfs_size ]]; then
  echo "ext4 sync persistence: failed to resolve rootfs partition" >&2
  exit 1
fi
recovery_image=$(mktemp .artifacts/tmp/ext4-sync-persistence.XXXXXX.ext4)
trap 'rm -f "$recovery_image"' EXIT
dd if=.artifacts/disk.img of="$recovery_image" bs=512 \
  skip="$rootfs_start" count="$rootfs_size" conv=sparse status=none

set +e
e2fsck -fy "$recovery_image" >"$log_dir/recovery-fsck.log" 2>&1
recovery_status=$?
set -e
cat "$log_dir/recovery-fsck.log"
if (( recovery_status > 1 )); then
  echo "ext4 sync persistence: recovery failed status=$recovery_status" >&2
  exit 1
fi
# A crash can leave only the non-journalled superblock-wide free-count
# aggregate stale; e2fsck recomputes that from otherwise consistent group
# descriptors and bitmaps.  A per-group count mismatch is different: it means
# the journalled allocation metadata disagrees with its bitmap and is rejected.
structural_pattern='^(Inode |Block bitmap differences|Inode bitmap differences|Directory inode|Entry |Unattached inode|Multiply-claimed|(Free (blocks|inodes)|Directories) count wrong for group)|UNEXPECTED INCONSISTENCY|filesystem still has errors|corrupt'
if rg -i "$structural_pattern" "$log_dir/recovery-fsck.log"; then
  echo "ext4 sync persistence: recovery required structural repair" >&2
  exit 1
fi
e2fsck -fn "$recovery_image" 2>&1 | tee "$log_dir/post-recovery-fsck.log"
echo "EXT4_SYNC_PERSISTENCE_HOST_FSCK=OK"
