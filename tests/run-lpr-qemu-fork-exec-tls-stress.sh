#!/usr/bin/env bash
# Stress regression: fork/exec must preserve a valid calling-thread TLS base,
# including when the parent has another live pthread.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

pkill -9 qemu-system-x86 2>/dev/null || true
.artifacts/bin/pacgo qemu-test \
  --timeout 90s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send '/cmd/lpr_fork_pthread_probe.elf --stress 128' \
  --expect 'FORK_EXEC_TLS_STRESS_START iterations=128' \
  --expect 'FORK_EXEC_TLS_STRESS_PROGRESS=32' \
  --expect 'FORK_EXEC_TLS_STRESS_PROGRESS=64' \
  --expect 'FORK_EXEC_TLS_STRESS_PROGRESS=96' \
  --expect 'FORK_EXEC_TLS_STRESS_PROGRESS=128' \
  --expect 'FORK_EXEC_TLS_STRESS=OK iterations=128'
