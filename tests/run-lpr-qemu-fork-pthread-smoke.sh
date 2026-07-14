#!/usr/bin/env bash
# Regression smoke: a forked child can create and join a pthread.
#
# This covers the parent baseline, a plain fork child, a fork child after
# killing and reaping a sibling, and fork while a parent worker remains live.
# The child cases protect the LPR invariant that fork discards the parent's
# thread registry and starts with one thread.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

pkill -9 qemu-system-x86 2>/dev/null || true
.artifacts/bin/pacgo qemu-test \
  --timeout 60s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send '/cmd/lpr_fork_pthread_probe.elf' \
  --expect 'FORK_PTHREAD_START' \
  --expect 'FORK_PTHREAD_PARENT=OK rc=0' \
  --expect 'FORK_PTHREAD_CHILD_PLAIN=OK rc=0' \
  --expect 'FORK_PTHREAD_CHILD_AFTER_KILL=OK rc=0' \
  --expect 'FORK_PTHREAD_CHILD_LIVE_WORKER=OK rc=0' \
  --expect 'FORK_PTHREAD_DONE'
