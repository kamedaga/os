#!/usr/bin/env bash
# Regression smoke: sequential SIGCHLD exits must interrupt the generic event
# loop through a real SA_ONSTACK signal frame and leave no stale child state.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send '/cmd/lpr_child_lifecycle_red.elf' \
  --expect 'LPR_CHILD_LIFECYCLE_RED_START' \
  --expect 'LPR_CHILD_LIFECYCLE_RED_ITERATION_1=OK' \
  --expect 'LPR_CHILD_LIFECYCLE_RED_ITERATION_2=OK' \
  --expect 'LPR_CHILD_LIFECYCLE_RED_NO_CHILDREN=OK' \
  --expect 'LPR_CHILD_LIFECYCLE_RED_DONE'
