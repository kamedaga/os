#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
iters="${DRM_RESTART_ITERS:-20}"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  rm -f .artifacts/cmake/*/*.elf
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi
pkill -9 qemu-system-x86 2>/dev/null || true
sleep 1
.artifacts/bin/pacgo qemu-test \
  --timeout 90s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send "bash /cmd/drm_restart_smoke.sh ${iters}" \
  --expect 'DRM_RESTART_ITER pass=1' \
  --expect "DRM_RESTART_ITER pass=${iters}" \
  --expect "DRM_RESTART_SMOKE_DONE iterations=${iters} delay_ms=0"

clean_state='\[drmd\] close handle=[0-9]+ status=0 state handles=0 fb=0 dumb=0 eventq=0 events=0 master=0'
clean_count="$(rg -c "$clean_state" .artifacts/serial-tty-test.log || true)"
[[ "$clean_count" == "${iters}" ]]
rg -q '\[drmd\] close handle=1 status=0 state handles=0 fb=0 dumb=0 eventq=0 events=0 master=0' .artifacts/serial-tty-test.log
rg '\[drmd\] close handle=[0-9]+' .artifacts/serial-tty-test.log | tail -n 1 | rg -q 'status=0 state handles=0 fb=0 dumb=0 eventq=0 events=0 master=0'
