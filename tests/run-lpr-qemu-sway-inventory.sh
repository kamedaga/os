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
  --send "M51_VARIANT=${M51_VARIANT:-default} M51_LAUNCH_MODE=${M51_LAUNCH_MODE:-compiled} bash /cmd/sway_inventory.sh" \
  --expect "M51_SEAT_SETUP launcher=${M51_LAUNCH_MODE:-compiled}" \
  --expect 'M51_SWAY_VERSION_STATUS=0' \
  --expect 'M53_UDEV_DISCOVERY_STATUS=0' \
  --expect 'major=226 minor=0 name=/dev/dri/card0' \
  --expect 'M53_UDEV_PROBE_STATUS=0' \
  --expect 'Initializing Wayland server' \
  --expect 'M51_SWAY_STATUS=' \
  --expect 'M51_SWAY_INVENTORY_DONE'
