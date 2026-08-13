#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

log_dir=.artifacts/test-results/apk-shell-smoke
mkdir -p "$log_dir" .artifacts/tmp

if [[ ${SKIP_SYNC:-0} != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --cpus 4 \
  --timeout 240s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --python tests/qemu_lpr_apk_shell_smoke.py

serial=.artifacts/serial-tty-test.log
python_log=.artifacts/qemu-tty-python.log
if ! rg -q '^APK_SHELL_QEMU=OK$' "$python_log"; then
  tail -n 160 "$python_log" >&2
  exit 1
fi
fault_pattern='GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault|EXT4-fs error|Out of memory|FILED_STORAGE_FAULT'
if rg -q "$fault_pattern" "$serial"; then
  rg -n "$fault_pattern" "$serial" >&2
  exit 1
fi
cp "$serial" "$log_dir/mutation-serial.log"
cp "$python_log" "$log_dir/mutation-console.log"

# The mutation phase ends with wget/fastfetch installed and nano removed.
# Start a new VM against the same ext4 rootfs to prove that both package DB
# state and the installed files survived sync plus cold boot.
.artifacts/bin/pacgo qemu-test \
  --cpus 4 \
  --timeout 60s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'if apk info -e wget >/dev/null 2>&1 && apk info -e fastfetch >/dev/null 2>&1 && ! apk info -e nano >/dev/null 2>&1 && test -x /usr/bin/wget && test -x /usr/bin/fastfetch; then sync && echo APK_SHELL_PERSISTENCE=OK; else echo APK_SHELL_PERSISTENCE=FAIL; fi' \
  --expect 'APK_SHELL_PERSISTENCE=OK'

if rg -q "$fault_pattern" "$serial"; then
  rg -n "$fault_pattern" "$serial" >&2
  exit 1
fi
cp "$serial" "$log_dir/persistence-serial.log"
cp .artifacts/console-tty-test.log "$log_dir/persistence-console.log"

# qemu-test stops QEMU from the host after the success marker, so this is an
# intentional crash image, not a cleanly unmounted filesystem.  Validate the
# normal ext4 recovery path on a disposable partition copy, then require a
# fully clean read-only fsck.  Structural repair is never accepted here.
rootfs_spec=$(sfdisk -d .artifacts/disk.img | awk '/name="rootfs"/ { print; exit }')
rootfs_start=$(printf '%s\n' "$rootfs_spec" | sed -n 's/.*start= *\([0-9][0-9]*\),.*/\1/p')
rootfs_size=$(printf '%s\n' "$rootfs_spec" | sed -n 's/.*size= *\([0-9][0-9]*\),.*/\1/p')
if [[ -z $rootfs_start || -z $rootfs_size ]]; then
  echo "apk shell smoke: failed to resolve rootfs partition" >&2
  exit 1
fi
recovery_image=$(mktemp .artifacts/tmp/apk-shell-recovery.XXXXXX.ext4)
trap 'rm -f "$recovery_image"' EXIT
dd if=.artifacts/disk.img of="$recovery_image" bs=512 \
  skip="$rootfs_start" count="$rootfs_size" conv=sparse status=none

set +e
e2fsck -fy "$recovery_image" >"$log_dir/recovery-fsck.log" 2>&1
recovery_status=$?
set -e
cat "$log_dir/recovery-fsck.log"
if (( recovery_status > 1 )); then
  echo "apk shell smoke: ext4 recovery failed status=$recovery_status" >&2
  exit 1
fi
# A host-stopped read-write mount may leave only the non-journalled,
# superblock-wide free-count aggregate stale.  Per-group count mismatches are
# journalled allocation-metadata damage and must fail the acceptance test.
structural_pattern='^(Inode |Block bitmap differences|Inode bitmap differences|Directory inode|Entry |Unattached inode|Multiply-claimed|(Free (blocks|inodes)|Directories) count wrong for group)|UNEXPECTED INCONSISTENCY|filesystem still has errors|corrupt'
if rg -i "$structural_pattern" "$log_dir/recovery-fsck.log"; then
  echo "apk shell smoke: ext4 recovery required structural repair" >&2
  exit 1
fi
e2fsck -fn "$recovery_image" 2>&1 | tee "$log_dir/post-recovery-fsck.log"
