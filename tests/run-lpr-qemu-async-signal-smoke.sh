#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

bash tests/run-termd-pgrp-signal-unit.sh

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  rm -f .artifacts/cmake/*/*.elf
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send '. /cmd/lpr_async_signal_smoke.sh' \
  --expect 'ASYNC_SIGNAL_START' \
  --expect 'ASYNC_SIGINT_DEFAULT=OK' \
  --expect 'ASYNC_SIGKILL=OK' \
  --expect 'ASYNC_BUSYBOX_TIMEOUT=OK' \
  --expect 'ASYNC_HANDLER_CALLED' \
  --expect 'ASYNC_HANDLER_CONTINUED' \
  --expect 'ASYNC_CUSTOM_HANDLER=OK' \
  --expect 'ASYNC_ALTSTACK_ONSTACK=OK' \
  --expect 'ASYNC_ALTSTACK_CONTINUED' \
  --expect 'ASYNC_SIGALTSTACK=OK' \
  --expect 'ASYNC_EPOLL_HANDLER_READY' \
  --expect 'ASYNC_EPOLL_HANDLER_CALLED' \
  --expect 'ASYNC_EPOLL_HANDLER_CONTINUED' \
  --expect 'ASYNC_EPOLL_HANDLER=OK' \
  --expect 'ASYNC_SIGNAL_DONE failures=0'
