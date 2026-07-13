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
  --send 'bash /cmd/sway_shm_client.sh' \
  --expect 'Atomic modesetting unsupported, using legacy DRM interface' \
  --expect 'Creating GLES2 renderer' \
  --expect 'GL renderer: llvmpipe' \
  --expect "Running compositor on wayland display 'wayland-1'" \
  --expect 'M56_WL_SHM_TRANSFER bytes=196608' \
  --expect 'M56_WL_XDG_CONFIGURE_OK' \
  --expect 'M56_WL_SURFACE_READY color=#336699 size=256x192' \
  --expect 'M56_SHM_SYNC_DONE' \
  --screendump-device pachagpu \
  --screendump-check 'M56_WL_SURFACE_READY color=#336699 size=256x192@508,380,8,8=#336699'

if rg -Fq "failed to execute 'swaybg'" .artifacts/console-tty-test.log; then
  echo 'swaybg helper failure detected' >&2
  exit 1
fi
if rg -Fq 'Xwayland' .artifacts/console-tty-test.log; then
  echo 'unexpected Xwayland warning' >&2
  exit 1
fi
