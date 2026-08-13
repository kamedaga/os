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
  --input-profile keyboard-tablet \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_evdev_tablet_state_smoke.elf' \
  --expect 'TABLET_STATE_READY name=QEMU Virtio Tablet' \
  --expect 'TABLET_FIRST_PASS state=4096,8192' \
  --expect 'TABLET_STATE_PASS state=4096,16384 duplicate_x_filtered=1' \
  --input-send-event 'TABLET_STATE_READY name=QEMU Virtio Tablet@abs:x=4096,abs:y=8192' \
  --input-send-event 'TABLET_SECOND_READY unchanged_x=4096@abs:x=4096,abs:y=16384'
