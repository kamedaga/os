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
  --timeout 15s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/kms_modeset_smoke.sh' \
  --expect 'KMS_FRAME1_READY color=ff0000' \
  --expect 'KMS_FRAME2_READY color=00ffff' \
  --expect 'KMS_FRAME3_DIRTY_READY color=ff0000' \
  --expect 'KMS_MODESET_OK' \
  --screendump-check 'KMS_FRAME1_READY@8,8,8,8=#ff0000' \
  --screendump-check 'KMS_FRAME2_READY@8,8,8,8=#00ffff' \
  --screendump-check 'KMS_FRAME3_DIRTY_READY@8,8,8,8=#ff0000'
