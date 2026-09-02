#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 90s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_exec_pipe_race.elf' \
  --expect 'LPR_EXEC_PIPE_RACE_START' \
  --expect 'LPR_EXEC_PIPE_EOF_DONE' \
  --expect 'LPR_EXEC_PIPE_RACE_WINDOW iterations=' \
  --expect 'LPR_EXEC_PIPE_RACE_DONE'

serial=.artifacts/serial-tty-test.log

mapfile -t samples < <(
  sed -n 's/.*\[lpr\] state_checkpoint source=sync open=\([0-9][0-9]*\) live=\([0-9][0-9]*\) filed_status=0.*/\1 \2/p' "$serial"
)
if (( ${#samples[@]} != 1 )); then
  echo "exec pipe inherit smoke: expected 1 checkpoint, got ${#samples[@]}" >&2
  exit 1
fi
if [[ ${samples[0]} != '3 2' ]]; then
  echo "exec pipe inherit smoke: inherited FD/OFD count actual=[${samples[0]}] expected=[3 2]" >&2
  exit 1
fi

echo 'LPR_EXEC_PIPE_RESULT probe=[3 2]'
