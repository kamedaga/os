#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

# Identical busybox ls probes bracket 50 measured busybox ls executions. Every
# exec triggers the existing execve_begin dump, so the final 52 count samples are
# before probe + 50 iterations + after probe. Keep tty input to one line.
.artifacts/bin/pacgo qemu-test \
  --timeout 60s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/lpr_state_leak.sh' \
  --expect 'LPR_STATE_LEAK_BEFORE' \
  --expect 'LPR_STATE_LEAK_AFTER' \
  --expect 'LPR_STATE_LEAK_DONE'

serial=.artifacts/serial-tty-test.log
counts_hash=1788043886520475514
reason_hash=13796112191661380053

mapfile -t samples < <(
  awk -v event="a0=$counts_hash" -v reason="a5=$reason_hash" '
    index($0, event) && index($0, reason) {
      open_count = ""
      live_count = ""
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^a3=/) { split($i, value, "="); open_count = value[2] }
        if ($i ~ /^a4=/) { split($i, value, "="); live_count = value[2] }
      }
      if (open_count != "" && live_count != "") {
        print open_count, live_count
      }
    }
  ' "$serial"
)

if (( ${#samples[@]} < 52 )); then
  echo "state leak smoke: expected 52 lpr.state.counts samples, got ${#samples[@]}" >&2
  exit 1
fi
before_index=$((${#samples[@]} - 52))
after_index=$((${#samples[@]} - 1))
before="${samples[$before_index]}"
after="${samples[$after_index]}"
if [[ "$before" != "$after" ]]; then
  echo "state leak smoke: count mismatch before=[$before] after=[$after]" >&2
  exit 1
fi

read -r open_count live_object_count <<<"$after"
echo "LPR_STATE_LEAK_COUNTS open_count=$open_count live_object_count=$live_object_count"
