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
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send '/cmd/lpr_evdev_smoke.elf' \
  --expect 'EVDEV_METADATA_OK name=QEMU Virtio Keyboard' \
  --expect 'EVDEV_METADATA_OK name=QEMU Virtio Mouse' \
  --expect 'EVDEV_EVENT device=event0 type=1 code=30 value=1' \
  --expect 'EVDEV_EVENT device=event1 type=2 code=0 value=7' \
  --expect 'EVDEV_EVENT_PASS key=30:1,0 rel=0:7,1:-4 button=272:1,0' \
  --input-send-event 'EVDEV_READY keyboard=event0 mouse=event1@key:a=down,key:a=up,rel:x=7,rel:y=-4,btn:left=down,btn:left=up'
