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
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --python "$repo_root/tests/qemu_lpr_gnu_coreutils_smoke.py"
