#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 20s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/lpr_wait4_signal_smoke.sh' \
  --expect 'LPR_WAIT4_SIGNAL_READY child=' \
  --expect 'LPR_WAIT4_SIGNAL_TARGET_PRESENT pid=' \
  --expect 'LPR_WAIT4_SIGNAL_KILL status=0' \
  --expect 'LPR_WAIT4_SIGNAL_TRAP' \
  --expect 'LPR_WAIT4_SIGNAL_AFTER_WAIT status=' \
  --expect 'LPR_WAIT4_SIGNAL_PARENT_WAIT status=0' \
  --expect 'LPR_WAIT4_SIGNAL_DONE'

console=.artifacts/console-tty-test.log
serial=.artifacts/serial-tty-test.log

mapfile -t probes < <(
  sed -n 's/.*LPR_WAIT4_SIGNAL_LPR_PROBE stage=\([^ ]*\).*/\1/p' "$console"
)
if (( ${#probes[@]} != 2 )); then
  echo "wait4 signal smoke: expected 2 LPR probes, got ${#probes[@]}" >&2
  exit 1
fi

mapfile -t lpr_samples < <(
  sed -n 's/.*\[lpr\] state_checkpoint source=sync open=\([0-9][0-9]*\) live=\([0-9][0-9]*\) filed_status=0.*/\1 \2/p' "$serial"
)
if (( ${#lpr_samples[@]} != 2 )); then
  echo "wait4 signal smoke: expected 2 LPR state checkpoints, got ${#lpr_samples[@]}" >&2
  exit 1
fi

lpr_baseline=${lpr_samples[0]}
for ((i = 0; i < ${#lpr_samples[@]}; i++)); do
  sample=${lpr_samples[$i]}
  stage=${probes[$i]}
  if [[ $sample != "$lpr_baseline" ]]; then
    echo "wait4 signal smoke: LPR fd/OFD delta stage=$stage baseline=[$lpr_baseline] actual=[$sample]" >&2
    exit 1
  fi
done

echo "LPR_WAIT4_SIGNAL_RESULT lpr=[$lpr_baseline] interrupt=pass"
