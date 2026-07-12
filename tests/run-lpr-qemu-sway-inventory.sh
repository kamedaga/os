#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
fi
pkill -9 qemu-system-x86 || true
sleep 1
.artifacts/bin/pacgo qemu-test \
  --timeout 240s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send "M51_VARIANT=${M51_VARIANT:-default} bash /cmd/sway_inventory.sh" \
  --expect 'M51_SEAT_SETUP launcher=compiled' \
  --expect 'M51_SWAY_VERSION_STATUS=' \
  --expect 'M51_SWAY_STATUS=' \
  --expect 'M51_SWAY_INVENTORY_DONE'
