#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  rm -f .artifacts/cmake/*/*.elf
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi
GALLIUM_DRIVER="${VIRGL_HOST_DRIVER:-d3d12}" \
  .artifacts/bin/pacgo qemu-test \
  --console-shell \
  --graphics virgl \
  --display gtk \
  --qemu-arg=-vga \
  --qemu-arg=none \
  --qemu-arg=-device \
  --qemu-arg=ramfb \
  --timeout 90s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'busybox timeout -s KILL 30 /cmd/lpr_drm_prime_smoke.elf; printf "DRM_PRIME_RESULT status=%s\n" "$?"' \
  --expect 'PRIME_GBM_EXPORT_OK' \
  --expect 'PRIME_CHILD_IMPORT_OK' \
  --expect 'PRIME_CROSS_PROCESS_PIXELS_OK' \
  --expect 'PRIME_CROSS_PROCESS_DISPLAY_OK' \
  --expect 'PRIME_SMOKE_DONE' \
  --expect 'DRM_PRIME_RESULT status=0'
