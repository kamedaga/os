#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  rm -f .artifacts/cmake/*/*.elf
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi
.artifacts/bin/pacgo qemu-test \
  --timeout 90s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/drm_prime_smoke.sh' \
  --expect 'PRIME_GBM_EXPORT_OK' \
  --expect 'PRIME_CHILD_IMPORT_OK' \
  --expect 'PRIME_CROSS_PROCESS_DISPLAY_OK' \
  --expect 'PRIME_SMOKE_DONE' \
  --expect 'DRM_PRIME_RESULT status=0' \
  --screendump-check 'PRIME_CROSS_PROCESS_DISPLAY_OK@8,8,8,8=#00ffff' \
  --screendump-device pachagpu
