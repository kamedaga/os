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
  --timeout 240s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/mesa_inventory.sh' \
  --expect 'MESA_DRM_DEVICE_OK domain=0000 bus=00 dev=03 func=0 vendor=1af4 device=1050' \
  --expect 'MESA_CPU_AFFINITY online=4 configured=4' \
  --expect 'MESA_STAGE_A_PASS' \
  --expect 'MESA_STAGE_B_PASS' \
  --expect 'MESA_STAGE_C_PASS' \
  --expect 'MESA_STAGE_D_PASS swap=1' \
  --expect 'MESA_STAGE_E_PASS setcrtc=1 page_flip_flags=0 event_queue=not-used' \
  --expect 'MESA_RUN_RESULT stage=d variant=lp2 status=0' \
  --expect 'MESA_INVENTORY_DONE' \
  --screendump-check 'MESA_FRAME2_READY@8,8,8,8=#00ffff'
