#!/usr/bin/env bash
# Concurrent process-clone regression corresponding to GUI polling workloads.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

pkill -9 qemu-system-x86 2>/dev/null || true
.artifacts/bin/pacgo qemu-test \
  --timeout 120s \
  --qemu-arg=-m \
  --qemu-arg=4G \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_fork_pthread_probe.elf --parallel 4 256' \
  --expect 'FORK_EXEC_TLS_PARALLEL=OK workers=4 iterations=256'

if rg -n 'PAGE FAULT|USER fault|FORK_EXEC_TLS_(STRESS|PARALLEL)_FAIL' \
  .artifacts/serial-tty-test.log .artifacts/console-tty-test.log; then
  echo 'fork/exec/TLS parallel run contained a kernel fault or guest failure' >&2
  exit 1
fi
