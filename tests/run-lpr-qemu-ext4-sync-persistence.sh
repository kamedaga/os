#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

PACHA_EXT4_SYNC_PHASE=write \
  .artifacts/bin/pacgo qemu-test \
    --timeout 30s \
    --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
    --python "$repo_root/tests/qemu_lpr_ext4_sync_persistence.py"

PACHA_EXT4_SYNC_PHASE=read \
  .artifacts/bin/pacgo qemu-test \
    --timeout 30s \
    --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
    --python "$repo_root/tests/qemu_lpr_ext4_sync_persistence.py"
