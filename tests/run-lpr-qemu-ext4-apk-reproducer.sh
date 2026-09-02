#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

log_dir=.artifacts/test-results/ext4-apk-reproducer
mkdir -p "$log_dir"

if [[ ${SKIP_SYNC:-0} != 1 ]]; then
  rm -f "$repo_root/.artifacts/disk.img"
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs --force
fi

.artifacts/bin/pacgo qemu-test \
  --cpus 4 \
  --timeout 120s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '. /cmd/ext4_apk_reproducer.sh' \
  --expect 'EXT4_APK_REPRO_CREATE=OK files=512' \
  --expect 'EXT4_APK_REPRO_RENAME=OK files=256' \
  --expect 'EXT4_APK_REPRO_IMMUTABLE_AFTER_RENAME=OK' \
  --expect 'EXT4_APK_REPRO_TRUNCATE_UNLINK=OK files=256' \
  --expect 'EXT4_APK_REPRO_OPEN_UNLINK=OK' \
  --expect 'EXT4_APK_REPRO_PARALLEL=OK operations=256' \
  --expect 'EXT4_APK_REPRO_FSYNC=OK inventory=384' \
  --expect 'EXT4_APK_REPRO_CLEANUP=OK' \
  --expect 'EXT4_APK_REPRO_DONE'

serial=.artifacts/serial-tty-test.log
fault_pattern='GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault|EXT4-fs error|JBD2:|I/O error|Out of memory|FILED_STORAGE_FAULT'
if rg -q "$fault_pattern" "$serial"; then
  rg -n "$fault_pattern" "$serial" >&2
  exit 1
fi
cp "$serial" "$log_dir/serial.log"
cp .artifacts/console-tty-test.log "$log_dir/console.log"
cp .artifacts/qemu-tty-host-time.log "$log_dir/host-time.log"
