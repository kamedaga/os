#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

pkill -9 qemu-system-x86 || true
sleep 1
monitor="${M53B_MONITOR:-1}"
args=(
  --timeout "${M53B_TIMEOUT:-90s}"
  --boot-marker '[termd] linux tty hvc open ready index=0 handle='
  --send "M53B_MONITOR=${monitor} bash /cmd/m53b_monitor_shell.sh"
  --expect 'SHELL_INTERACTION_DONE failures=0'
  --expect 'M53B_SHELL_SECONDS='
  --expect 'M53B_MONITOR_SHELL_STATUS=0'
)
if [[ "$monitor" == "1" ]]; then
  args+=(--expect 'M53_UEVENT poll=1 action=change sysname=card0 devnode=/dev/dri/card0')
fi
.artifacts/bin/pacgo qemu-test "${args[@]}"
