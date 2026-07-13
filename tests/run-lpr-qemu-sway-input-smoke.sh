#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  rm -f .artifacts/userland-fixtures/lpr_wayland_shm_client.elf
  rm -f .artifacts/pack/build/lpr_wayland_shm_client.sha256
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi
pkill -9 qemu-system-x86 2>/dev/null || true
sleep 1
.artifacts/bin/pacgo qemu-test \
  --timeout 180s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/sway_input.sh' \
  --expect 'Atomic modesetting unsupported, using legacy DRM interface' \
  --expect 'Creating GLES2 renderer' \
  --expect 'GL renderer: llvmpipe' \
  --expect 'Starting libinput backend' \
  --expect 'Adding QEMU Virtio Keyboard' \
  --expect 'Adding QEMU Virtio Mouse' \
  --expect 'adding device 1575:1:QEMU_Virtio_Keyboard to seat seat0' \
  --expect 'adding device 1575:2:QEMU_Virtio_Mouse to seat seat0' \
  --expect 'Created keyboard group' \
  --expect "Running compositor on wayland display 'wayland-1'" \
  --expect 'M57_SEAT_NAME=seat0' \
  --expect 'M57_KEYMAP_OK format=1' \
  --expect 'M57_INPUT_READY seat=seat0 keyboard=1 pointer=1' \
  --expect 'M57_KEY code=30 state=1' \
  --expect 'M57_KEY code=30 state=0' \
  --expect 'M57_MOTION dx=7 dy=-4' \
  --expect 'M57_BUTTON btn=272 state=1' \
  --expect 'M57_BUTTON btn=272 state=0' \
  --expect 'M57_INPUT_PASS key=30/1/0 motion=7,-4 button=272/1/0' \
  --expect 'M57_INPUT_SYNC_DONE' \
  --input-send-event 'M57_INPUT_READY seat=seat0 keyboard=1 pointer=1@key:a=down,key:a=up,rel:x=7,rel:y=-4,btn:left=down,btn:left=up'

if rg -Fq 'Failed to create input event on event loop' .artifacts/console-tty-test.log; then
  echo 'libinput event-loop registration failed' >&2
  exit 1
fi
if rg -Fq 'Failed to compile keymap' .artifacts/console-tty-test.log; then
  echo 'XKB keymap compile failed' >&2
  exit 1
fi
