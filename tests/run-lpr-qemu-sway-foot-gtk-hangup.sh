#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 120s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/sway_foot_exit_acceptance.sh --gtk-hangup-test' \
  --expect 'SWAY_FOOT_GTK_HANGUP_READY' \
  --expect 'SWAY_FOOT_GTK_HANGUP_RESULT'

console=.artifacts/console-tty-test.log
if ! rg -q 'SWAY_FOOT_GTK_HANGUP_RESULT setup=ok gtk_alive=0 foot_window=0' "$console"; then
  rg -F 'SWAY_FOOT_GTK_HANGUP_RESULT' "$console" >&2 || true
  exit 1
fi

serial=.artifacts/serial-tty-test.log
if rg -q 'GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault' "$serial"; then
  rg -n 'GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault' "$serial" >&2
  exit 1
fi

echo 'SWAY_FOOT_GTK_HANGUP_PASS foot=closed gtk=gone'
