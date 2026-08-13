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
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/sway_socket_repeat.sh' \
  --expect 'M52_SOCKET_ITERATION=20 stale=0' \
  --expect 'M52_SOCKET_REPEAT_STATUS=0 completed=20'
