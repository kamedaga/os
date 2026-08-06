#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

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
fault_pattern='GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault|EXT4-fs error|Out of memory'
if rg -q "$fault_pattern" "$serial"; then
  rg -n "$fault_pattern" "$serial" >&2
  exit 1
fi
