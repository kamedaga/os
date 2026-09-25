#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
iters="${DRM_FLIP_ITERS:-20}"
frames="${DRM_CUBE_FRAMES:-8}"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  rm -f .artifacts/cmake/*/*.elf
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi
pkill -9 qemu-system-x86 2>/dev/null || true
sleep 1
GALLIUM_DRIVER="${VIRGL_HOST_DRIVER:-d3d12}" \
  .artifacts/bin/pacgo qemu-test \
  --console-shell \
  --graphics virgl \
  --display gtk \
  --qemu-arg=-vga \
  --qemu-arg=none \
  --qemu-arg=-device \
  --qemu-arg=ramfb \
  --timeout 210s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send "bash /cmd/drm_page_flip_smoke.sh ${iters} ${frames}" \
  --expect 'FLIP_EVENT_FRAME frame=1' \
  --expect "FLIP_EVENT_FRAME frame=${iters}" \
  --expect "FLIP_EVENT_PASS iterations=${iters}" \
  --expect 'CUBE_INIT renderer=virgl (D3D12 (' \
  --expect 'CUBE_FENCE_PASS api=glFinish' \
  --expect 'CUBE_PIXEL_CHECKSUM algorithm=fnv1a64' \
  --expect "CUBE_SCANOUT_PASS frame=${frames} event=${frames} rgba=0,255,0,255" \
  --expect 'CUBE_FRAME_READY frame=1 phase=front-red' \
  --expect "CUBE_FRAME_READY frame=${frames} phase=side-green" \
  --expect "CUBE_ANIMATION_PASS frames=${frames} events=${frames}" \
  --expect "DRM_PAGE_FLIP_SMOKE_DONE iterations=${iters} cube_frames=${frames}"
