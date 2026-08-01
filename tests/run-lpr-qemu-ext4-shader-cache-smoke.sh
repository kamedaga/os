#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
source tests/lib/pacgo_image_lock.sh

parts="${EXT4_SHADER_CACHE_PARTS:-8}"
case "$parts" in
  ''|*[!0-9]*|0)
    echo "ext4 shader cache smoke: invalid EXT4_SHADER_CACHE_PARTS=[$parts]" >&2
    exit 2
    ;;
esac
if (( parts > 50 )); then
  echo "ext4 shader cache smoke: EXT4_SHADER_CACHE_PARTS exceeds 50: $parts" >&2
  exit 2
fi
last_part=$((parts - 1))

log_dir="${EXT4_SHADER_CACHE_LOG_DIR:-.artifacts/test-results/ext4-shader-cache-smoke}"
mkdir -p "$log_dir"

pacgo_remove_image_locked .artifacts/disk.img
.artifacts/bin/pacgo sync rootfs --force
.artifacts/bin/pacgo sync bootfs

qemu_status=0
.artifacts/bin/pacgo qemu-test \
  --timeout 180s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send "/cmd/lpr_ext4_shader_cache_smoke.elf $parts" \
  --expect "EXT4_SHADER_CACHE_START parts=$parts header_bytes=20" \
  --expect "EXT4_SHADER_CACHE_PART=$last_part status=OK backend_pwrite_offset=0 bytes=20 files=2" \
  --expect 'EXT4_SHADER_CACHE_UNLINK=OK bytes=1664 order=open-unlink-ftruncate-write-fsync-close' \
  --expect "EXT4_SHADER_CACHE_DONE parts=$parts completed=$parts failures=0" || qemu_status=$?

serial=.artifacts/serial-tty-test.log
console=.artifacts/console-tty-test.log
serial_status=0
if [[ -f "$serial" ]]; then
  cp "$serial" "$log_dir/serial.log"
else
  echo "ext4 shader cache smoke: serial log missing" >&2
  serial_status=1
fi
if [[ -f "$console" ]]; then
  cp "$console" "$log_dir/console.log"
fi

forbidden_status=0
if [[ "$serial_status" -eq 0 ]] && grep -E \
  'EXT4-fs (error|warning)|ext4_lookup|\[filed\].*fatal|fatal stage=serve|lookup failure|EXT4_SHADER_CACHE_[A-Z_]+=FAIL' \
  "$serial" >"$log_dir/forbidden-serial.log"; then
  echo "ext4 shader cache smoke: forbidden serial diagnostics found" >&2
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
echo "EXT4_SHADER_CACHE_HOST_FSCK=OK parts=$parts"
