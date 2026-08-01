#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
source tests/lib/pacgo_image_lock.sh

iterations="${EXT4_UNLINK_JOURNAL_ITERS:-40}"
case "$iterations" in
  ''|*[!0-9]*|0)
    echo "ext4 unlink journal smoke: invalid EXT4_UNLINK_JOURNAL_ITERS=[$iterations]" >&2
    exit 2
    ;;
esac

log_dir=.artifacts/test-results/ext4-unlink-journal
mkdir -p "$log_dir"

pacgo_remove_image_locked .artifacts/disk.img
.artifacts/bin/pacgo sync rootfs --force
.artifacts/bin/pacgo sync bootfs

qemu_status=0
.artifacts/bin/pacgo qemu-test \
  --timeout 180s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send "bash /cmd/ext4_unlink_journal_smoke.sh $iterations" \
  --expect "EXT4_UNLINK_JOURNAL_START iterations=$iterations" \
  --expect "EXT4_UNLINK_JOURNAL_ITERATION=$iterations status=OK" \
  --expect "EXT4_UNLINK_JOURNAL_DONE iterations=$iterations completed=$iterations failures=0" \
  --expect "EXT4_UNLINK_JOURNAL_SOCKET_ITERATION=$iterations status=OK" \
  --expect "EXT4_UNLINK_JOURNAL_SOCKET_DONE iterations=$iterations failures=0" || qemu_status=$?

serial=.artifacts/serial-tty-test.log
serial_status=0
if [[ -f "$serial" ]]; then
  cp "$serial" "$log_dir/serial.log"
else
  echo "ext4 unlink journal smoke: serial log missing" >&2
  serial_status=1
fi
forbidden_status=0
if [[ "$serial_status" -eq 0 ]] && grep -E \
  'EXT4-fs (error|warning)|ext4_lookup|\[filed\].*fatal|fatal stage=serve|lookup failure|EXT4_UNLINK_JOURNAL_(CREATE|LOOKUP|UNLINK|ENOENT|RECREATE|CLEANUP|SOCKET_[A-Z_]+)=FAIL' \
  "$serial" >"$log_dir/forbidden-serial.log"; then
  echo "ext4 unlink journal smoke: forbidden serial diagnostics found" >&2
  cat "$log_dir/forbidden-serial.log" >&2
  forbidden_status=1
fi

fsck_status=0
.artifacts/bin/pacgo fsck rootfs 2>&1 | tee "$log_dir/e2fsck.log" || fsck_status=$?
if [[ "$qemu_status" -ne 0 ]]; then
  exit "$qemu_status"
fi
if [[ "$serial_status" -ne 0 || "$forbidden_status" -ne 0 || "$fsck_status" -ne 0 ]]; then
  exit 1
fi
echo "EXT4_UNLINK_JOURNAL_HOST_FSCK=OK iterations=$iterations"
