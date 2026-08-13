#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  rm -f .artifacts/cmake/*/*.elf
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi
pkill -9 qemu-system-x86 2>/dev/null || true
sleep 1
.artifacts/bin/pacgo qemu-test \
  --timeout 120s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_libinput_seatd_smoke.elf' \
  --expect 'LIBSEAT_READY name=seat0' \
  --expect 'LIBINPUT_READY backend=path-seatd devices=2' \
  --expect 'LIBINPUT_KEY code=30 state=1' \
  --expect 'LIBINPUT_MOTION dx=7.0 dy=0.0' \
  --expect 'LIBINPUT_MOTION dx=0.0 dy=-4.0' \
  --expect 'LIBINPUT_BUTTON code=272 state=1' \
  --expect 'LIBINPUT_EVENT_PASS key=30 motion=7,-4 button=272 seat=seat0' \
  --input-send-event 'LIBINPUT_READY backend=path-seatd devices=2@key:a=down,key:a=up,rel:x=7,rel:y=-4,btn:left=down,btn:left=up'
