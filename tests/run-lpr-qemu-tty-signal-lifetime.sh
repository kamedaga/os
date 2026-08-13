#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 50s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --python "$repo_root/tests/qemu_lpr_tty_signal_lifetime.py"
