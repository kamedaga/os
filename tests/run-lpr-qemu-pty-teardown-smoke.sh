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
  --timeout 15s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/pty_teardown_smoke.sh' \
  --expect 'PTY_TEARDOWN_GREP_1=OK' \
  --expect 'PTY_TEARDOWN_SLEEP_1=OK' \
  --expect 'PTY_TEARDOWN_GREP_2=OK' \
  --expect 'PTY_TEARDOWN_SLEEP_2=OK' \
  --expect 'PTY_TEARDOWN_GREP_3=OK' \
  --expect 'PTY_TEARDOWN_SLEEP_3=OK' \
  --expect 'PTY_TEARDOWN_DONE failures=0'
