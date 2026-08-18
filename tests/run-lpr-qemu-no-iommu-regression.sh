#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  find .artifacts/cmake -mindepth 2 -maxdepth 2 -type f -name '*.elf' -delete
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi
pkill -9 qemu-system-x86 2>/dev/null || true
sleep 2
.artifacts/bin/pacgo qemu-test \
  --no-iommu \
  --timeout 150s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_evdev_smoke.elf; bash /cmd/ext4_w.sh; bash /cmd/ext4_r.sh' \
  --expect 'vtd: mode=pass-through reason=dmar-not-found active=0 faults=0' \
  --expect 'EVDEV_METADATA_OK name=QEMU Virtio Keyboard' \
  --expect 'EVDEV_METADATA_OK name=QEMU Virtio Mouse' \
  --expect 'EVDEV_EVENT_PASS key=30:1,0 rel=0:7,1:-4 button=272:1,0' \
  --expect 'EVDEV_BACKLOG_PASS events=3 reads=3 level_rearm=1' \
  --expect 'EVDEV_EPOLL_PASS repeated_ready=1 min_events=3 drained_ready=0' \
  --expect 'EXT4W_FILE=OK' \
  --expect 'EXT4W_DIR=OK' \
  --expect 'EXT4R_FILE=OK' \
  --expect 'EXT4R_DIR=OK' \
  --expect 'EXT4R_CLEAN=OK' \
  --expect 'EXT4R_SYNC=OK' \
  --input-send-event 'EVDEV_KEY_READY device=event0@key:a=down,key:a=up' \
  --input-send-event 'EVDEV_MOUSE_READY device=event1@rel:x=7,rel:y=-4,btn:left=down,btn:left=up' \
  --input-send-event 'EVDEV_BACKLOG_READY device=event1 read_capacity=1@rel:x=1,rel:y=1,btn:left=down,btn:left=up' \
  --input-send-event 'EVDEV_EPOLL_READY device=event1 read_capacity=1@rel:x=2,rel:y=2,btn:left=down,btn:left=up'
