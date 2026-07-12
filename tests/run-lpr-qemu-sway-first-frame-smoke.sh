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
  --timeout 90s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/sway_first_frame.sh' \
  --expect 'Atomic modesetting unsupported, using legacy DRM interface' \
  --expect 'Found 1 DRM CRTCs' \
  --expect 'Found 1 DRM planes' \
  --expect 'GL renderer: llvmpipe' \
  --expect 'Created GBM allocator with backend drm' \
  --expect "Allocated 1024x768 GBM buffer with format XR24" \
  --expect 'M55_SWAY_FIRST_FRAME_READY' \
  --screendump-device pachagpu \
  --screendump-check 'M55_SWAY_FIRST_FRAME_READY@256,192,16,16=#000000'

rg -Fq '[drmd] kms framebuffer pool format=XR24 active=2' .artifacts/serial-tty-test.log
rg -Fq '[drmd] legacy page-flip events delivered count=2 crtc=51' .artifacts/serial-tty-test.log
if rg -Fq 'Failed to allocate shm file for format table' .artifacts/console-tty-test.log; then
  echo 'unexpected Wayland format-table shm allocator failure' >&2
  exit 1
fi
