#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_epoll_smoke.elf' \
  --expect 'LPR_EPOLL_START' \
  --expect 'LPR_EPOLL_MIXED=OK' \
  --expect 'LPR_EPOLL_LT=OK' \
  --expect 'LPR_EPOLL_DEL=OK' \
  --expect 'LPR_EPOLL_CLOEXEC=OK' \
  --expect 'LPR_EPOLL_ET_REJECT=OK' \
  --expect 'LPR_EPOLL_TIMEOUT=OK' \
  --expect 'LPR_EPOLL_DUP=OK' \
  --expect 'LPR_EPOLL_CLOSE_AUTO=OK' \
  --expect 'LPR_EPOLL_HUP=OK' \
  --expect 'LPR_EPOLL_NESTED=OK' \
  --expect 'LPR_EPOLL_FORK_INFINITE=OK' \
  --expect 'LPR_EPOLL_CLOEXEC_EXEC=OK' \
  --expect 'LPR_EPOLL_DONE'
