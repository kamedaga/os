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
  --cpus 4 \
  --timeout 30s \
  --console-shell \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '. /cmd/lpr_async_signal_smoke.sh' \
  --expect 'ASYNC_SIGNAL_START' \
  --expect 'ASYNC_SIGINT_DEFAULT=OK' \
  --expect 'ASYNC_SIGKILL=OK' \
  --expect 'ASYNC_BUSYBOX_TIMEOUT=OK' \
  --expect 'ASYNC_SSE_STACK=OK' \
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
  --expect 'ASYNC_SIGNALFD_READY' \
  --expect 'ASYNC_SIGNALFD_EPOLL=OK' \
  --expect 'ASYNC_SIGNALFD=OK' \
  --expect 'ASYNC_EXEC_SIGIGN=OK' \
  --expect 'ASYNC_EXEC_CAUGHT_RESET=OK' \
  --expect 'ASYNC_EXEC_SIGMASK=OK' \
  --expect 'ASYNC_EXEC_SIGNAL_STATE=OK' \
  --expect 'ASYNC_SIGTIMEDWAIT_PENDING=OK' \
  --expect 'ASYNC_SIGTIMEDWAIT_ZERO_TIMEOUT=OK' \
  --expect 'ASYNC_SIGTIMEDWAIT_FINITE_TIMEOUT=OK' \
  --expect 'ASYNC_SIGTIMEDWAIT_SIGCHLD=OK' \
  --expect 'ASYNC_SIGTIMEDWAIT_EINTR=OK' \
  --expect 'ASYNC_SIGTIMEDWAIT_IGNORED_RESTART=OK' \
  --expect 'ASYNC_SIGNAL_DONE failures=0'
