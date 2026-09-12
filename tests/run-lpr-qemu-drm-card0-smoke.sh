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
  --console-shell \
  --timeout 180s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/drm_card0_smoke.sh' \
  --expect '[gpud] ready generation=1' \
  --expect 'DRM_RENDER_OK name=virtio_gpu' \
  --expect 'version=' \
  --expect 'prime=3' \
  --expect 'dup=1 fork_lease=1 last_close=1 client_kill=1 reopen_client=1' \
  --expect '[gpud] drm client-hangup' \
  --expect 'backend-close=1'
