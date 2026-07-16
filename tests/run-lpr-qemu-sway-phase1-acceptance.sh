#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 420s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/sway_phase1_acceptance.sh' \
  --expect 'SWAY_PHASE1_SESSION_OK runtime=/run/user/0 seatd=persistent environment=clean' \
  --expect 'SWAY_PHASE1_RENDERER threaded_llvmpipe=1' \
  --expect 'SWAY_PHASE1_FRAME_READY iteration=1 mode=normal' \
  --expect 'SWAY_PHASE1_LIFECYCLE_OK iteration=3 mode=term' \
  --expect 'SWAY_PHASE1_LIFECYCLE_OK iteration=5 mode=kill' \
  --expect 'SWAY_PHASE1_LIFECYCLE_OK iteration=10 mode=kill' \
  --expect 'SWAY_PHASE1_ACCEPTANCE_PASS iterations=10 lifecycle=normal,term,kill foot_pty=1 environment=clean' \
  --screendump-device pachagpu \
  --screendump-check 'SWAY_PHASE1_FRAME_READY iteration=1 mode=normal@512,384,8,8=#242424'

console=.artifacts/console-tty-test.log
serial=.artifacts/serial-tty-test.log

for forbidden in \
  'PAGE FAULT' \
  'EGL_ANDROID_native_fence_sync is required' \
  'no such FD' \
  'SWAY_PHASE1_WATCHDOG_ESCALATED' \
  'SWAY_PHASE1_ACCEPTANCE_FAIL'
do
  if rg -Fq "$forbidden" "$console" "$serial"; then
    echo "Phase 1 acceptance: forbidden marker [$forbidden]" >&2
    exit 1
  fi
done
if ! rg -Fq 'GL renderer: llvmpipe' "$console" ||
   rg -Fq 'lpr_sway_launcher' "$console" "$serial"; then
  echo 'Phase 1 acceptance: direct threaded llvmpipe process contract missing' >&2
  exit 1
fi

mapfile -t filed_samples < <(
  sed -n 's/.*\[filed\] state_checkpoint source=client_sync active_handles=\([0-9][0-9]*\) active_sessions=\([0-9][0-9]*\).*/\1 \2/p' "$serial"
)
if (( ${#filed_samples[@]} != 11 )); then
  echo "Phase 1 acceptance: expected 11 Filed samples, got ${#filed_samples[@]}" >&2
  exit 1
fi
filed_baseline=${filed_samples[0]}
for sample in "${filed_samples[@]}"; do
  if [[ $sample != "$filed_baseline" ]]; then
    echo "Phase 1 acceptance: Filed resource delta baseline=[$filed_baseline] actual=[$sample]" >&2
    exit 1
  fi
done

mapfile -t probes < <(
  sed -n 's/.*SWAY_PHASE1_LPR_PROBE stage=\([^ ]*\) pid=\([0-9][0-9]*\).*/\1 \2/p' "$console"
)
if (( ${#probes[@]} != 11 )); then
  echo "Phase 1 acceptance: expected 11 LPR probes, got ${#probes[@]}" >&2
  exit 1
fi
counts_hash=1788043886520475514
lpr_baseline=
for probe in "${probes[@]}"; do
  read -r stage pid <<<"$probe"
  sample=$(
    awk -v event="a0=$counts_hash" -v owner="a1=$pid" '
      {
        event_match = 0
        owner_match = 0
        for (i = 1; i <= NF; i++) {
          event_match = event_match || $i == event
          owner_match = owner_match || $i == owner
        }
      }
      event_match && owner_match {
        open_count = ""
        live_count = ""
        for (i = 1; i <= NF; i++) {
          if ($i ~ /^a3=/) { split($i, value, "="); open_count = value[2] }
          if ($i ~ /^a4=/) { split($i, value, "="); live_count = value[2] }
        }
        if (open_count != "" && live_count != "") print open_count, live_count
      }
    ' "$serial" | tail -n 1
  )
  if [[ -z $sample ]]; then
    echo "Phase 1 acceptance: missing LPR sample stage=$stage pid=$pid" >&2
    exit 1
  fi
  if [[ -z $lpr_baseline ]]; then
    lpr_baseline=$sample
  elif [[ $sample != "$lpr_baseline" ]]; then
    echo "Phase 1 acceptance: LPR fd/OFD delta stage=$stage baseline=[$lpr_baseline] actual=[$sample]" >&2
    exit 1
  fi
done

drmd_zero=$(rg -c '\[drmd\] close .* state handles=0 fb=0 dumb=0 eventq=0 events=0 master=0' "$serial" || true)
if (( drmd_zero < 10 )); then
  echo "Phase 1 acceptance: expected 10 clean drmd closes, got $drmd_zero" >&2
  exit 1
fi

echo "SWAY_PHASE1_ACCEPTANCE_RESULT iterations=10 filed=[$filed_baseline] lpr=[$lpr_baseline] drmd_clean=$drmd_zero first_frame=#242424 lifecycle=normal,term,kill"
