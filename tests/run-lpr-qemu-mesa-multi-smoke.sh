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
  --timeout 180s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'LIBGL_DEBUG=verbose EGL_LOG_LEVEL=warning busybox timeout -s KILL 120 /cmd/lpr_mesa_multi_smoke.elf' \
  --expect 'MESA_MULTI_INIT role=0 node=card0 renderer=virgl (D3D12 (' \
  --expect 'MESA_MULTI_INIT role=1 node=renderD128 renderer=virgl (D3D12 (' \
  --expect 'MESA_MULTI_SHARED role=0 frames=4 pixels=38552 dmabuf_scm=1 sync_file_scm=4 peer=ok' \
  --expect 'MESA_MULTI_SHARED role=1 frames=4 pixels=38552 dmabuf_scm=1 sync_file_scm=4 peer=ok' \
  --expect 'MESA_MULTI_KILL signal=9 child-reaped=1 held_dmabuf_fds=2' \
  --expect '[gpud] drm client-hangup' \
  --expect 'generation=1 backend-close=1' \
  --expect 'MESA_MULTI_SURVIVOR retained_peer=1 draw_after_kill=1 fence=1 peer_fd=1' \
  --expect 'MESA_MULTI_INIT role=2 node=renderD128 renderer=virgl (D3D12 (' \
  --expect 'MESA_MULTI_REOPEN draw=1 fence=1 pixels=4819' \
  --expect 'MESA_MULTI_DONE clients=3 dmabuf_scm=2 sync_file_scm=8 client_kill=1 reopen=1'
