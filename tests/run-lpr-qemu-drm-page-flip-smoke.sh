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
.artifacts/bin/pacgo qemu-test \
  --timeout 210s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send "bash /cmd/drm_page_flip_smoke.sh ${iters} ${frames}" \
  --expect 'FLIP_EVENT_FRAME frame=1' \
  --expect "FLIP_EVENT_FRAME frame=${iters}" \
  --expect "FLIP_EVENT_PASS iterations=${iters}" \
  --expect 'CUBE_INIT renderer=llvmpipe' \
  --expect 'CUBE_FRAME_READY frame=1 phase=front-red' \
  --expect "CUBE_FRAME_READY frame=${frames} phase=side-green" \
  --expect "CUBE_ANIMATION_PASS frames=${frames} events=${frames}" \
  --expect "DRM_PAGE_FLIP_SMOKE_DONE iterations=${iters} cube_frames=${frames}" \
  --screendump-device pachagpu \
  --screendump-check "FLIP_EVENT_PASS iterations=${iters}@8,8,8,8=#ff00ff" \
  --screendump-check 'CUBE_FRAME_READY frame=1@508,380,8,8=#ff0000:4' \
  --screendump-check "CUBE_FRAME_READY frame=${frames}@508,380,8,8=#00ff00:4"
