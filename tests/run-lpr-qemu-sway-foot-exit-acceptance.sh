#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 120s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/sway_foot_exit_acceptance.sh' \
  --expect 'SWAY_FOOT_EXIT_CHILD_READY' \
  --expect 'SWAY_FOOT_EXIT_RESULT'

console=.artifacts/console-tty-test.log
if ! rg -Fq 'SWAY_FOOT_EXIT_RESULT setup=ok shell_alive=0 foot_alive=0' "$console"; then
  rg -F 'SWAY_FOOT_EXIT_RESULT' "$console" >&2 || true
  exit 1
fi

echo 'SWAY_FOOT_EXIT_ACCEPTANCE_PASS child=exit shell=gone foot=gone'
