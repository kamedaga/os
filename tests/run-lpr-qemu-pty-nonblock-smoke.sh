#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

cpus="${PTY_NONBLOCK_CPUS:-4}"
case "$cpus" in
  1|4) ;;
  *) echo "PTY_NONBLOCK_CPUS must be 1 or 4" >&2; exit 2 ;;
esac

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --cpus "$cpus" \
  --input-profile keyboard-tablet \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_pty_nonblock_probe.elf' \
  --expect 'lpr_pty_nonblock_probe: stage=empty-read-return result=-1 errno=11' \
  --expect 'lpr_pty_nonblock_probe: ok'
