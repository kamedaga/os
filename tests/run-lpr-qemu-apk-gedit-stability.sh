#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

log_dir=.artifacts/test-results/apk-gedit-stability
disk_image="$repo_root/.artifacts/disk.img"
mkdir -p "$log_dir"

fault_pattern='GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault|TableFull|anon_full|mem: refused|EXT4-fs error|JBD2:|I/O error|Out of memory|FILED_STORAGE_FAULT|kobox rwsem: long wait|kobox bitwait: long wait|kobox-fs: long wait'

for run in 1 2 3 4 5 6; do
  rm "$disk_image"
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs --force

  .artifacts/bin/pacgo qemu-test \
    --cpus 4 \
    --timeout 90s \
    --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
    --python tests/qemu_lpr_apk_gedit.py

  serial=.artifacts/serial-tty-test.log
  python_log=.artifacts/qemu-tty-python.log
  if ! rg -q '^APK_GEDIT_QEMU=OK$' "$python_log"; then
    tail -n 160 "$python_log" >&2
    exit 1
  fi
  if rg -q "$fault_pattern" "$serial"; then
    rg -n "$fault_pattern" "$serial" >&2
    exit 1
  fi
  cp "$serial" "$log_dir/run-${run}-serial.log"
  cp "$python_log" "$log_dir/run-${run}-console.log"
  cp .artifacts/qemu-tty-host-time.log "$log_dir/run-${run}-host-time.log"
  printf 'APK_GEDIT_STABILITY_RUN=%s OK\n' "$run"
done

printf 'APK_GEDIT_STABILITY=OK runs=6\n'
