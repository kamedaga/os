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
  --send 'bash /cmd/lpr_pthread_smoke.sh' \
  --expect 'LPR_PTHREAD_START' \
  --expect 'LPR_PTHREAD_CREATE_JOIN=OK' \
  --expect 'LPR_PTHREAD_MUTEX=OK' \
  --expect 'LPR_PTHREAD_COND=OK' \
  --expect 'LPR_PTHREAD_DETACHED_EXIT=OK' \
  --expect 'LPR_PTHREAD_POST_DETACHED_CREATE_JOIN=OK' \
  --expect 'LPR_PTHREAD_STATIC=OK' \
  --expect 'LPR_PTHREAD_DYNAMIC=OK' \
  --expect 'LPR_PTHREAD_DONE'
