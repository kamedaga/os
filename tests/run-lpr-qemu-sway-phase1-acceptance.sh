#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
fi

if [[ "${SKIP_QEMU:-0}" != 1 ]]; then
  .artifacts/bin/pacgo qemu-test \
    --timeout 900s \
    --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
    --send 'bash /cmd/sway_phase1_acceptance.sh' \
    --expect 'SWAY_PHASE1_SESSION_OK runtime=/run/user/0 seatd=persistent environment=clean' \
    --expect 'SWAY_PHASE1_RENDERER threaded_llvmpipe=1' \
    --expect 'SWAY_PHASE1_FRAME_READY iteration=1 mode=normal' \
    --expect 'SWAY_PHASE1_LIFECYCLE_OK iteration=3 mode=term' \
    --expect 'SWAY_PHASE1_LIFECYCLE_OK iteration=5 mode=kill' \
    --expect 'SWAY_PHASE1_ACCEPTANCE_PASS iterations=5 lifecycle=normal,term,kill foot_pty=1 environment=clean' \
    --screendump-device pachagpu \
    --screendump-check 'SWAY_PHASE1_FRAME_READY iteration=1 mode=normal@512,384,8,8=#242424'
fi

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
if (( ${#filed_samples[@]} != 6 )); then
  echo "Phase 1 acceptance: expected 6 Filed samples, got ${#filed_samples[@]}" >&2
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
  sed -n 's/.*SWAY_PHASE1_LPR_PROBE stage=\([^ ]*\).*/\1/p' "$console"
)
if (( ${#probes[@]} != 6 )); then
  echo "Phase 1 acceptance: expected 6 LPR probes, got ${#probes[@]}" >&2
  exit 1
fi
mapfile -t lpr_samples < <(
  sed -n 's/.*\[lpr\] state_checkpoint source=sync open=\([0-9][0-9]*\) live=\([0-9][0-9]*\) filed_status=0.*/\1 \2/p' "$serial"
)
if (( ${#lpr_samples[@]} != 6 )); then
  echo "Phase 1 acceptance: expected 6 LPR state checkpoints, got ${#lpr_samples[@]}" >&2
  exit 1
fi
lpr_baseline=${lpr_samples[0]}
for ((i = 0; i < ${#lpr_samples[@]}; i++)); do
  sample=${lpr_samples[$i]}
  stage=${probes[$i]}
  if [[ $sample != "$lpr_baseline" ]]; then
    echo "Phase 1 acceptance: LPR fd/OFD delta stage=$stage baseline=[$lpr_baseline] actual=[$sample]" >&2
    exit 1
  fi
done

if rg -q '\[drmd\] (close failed|retaining failed)' "$serial"; then
  echo 'Phase 1 acceptance: drmd reported a failed resource release' >&2
  exit 1
fi
drmd_reaps=$(rg -c '\[drmd\] orphan reap .* status=0' "$serial" || true)
drmd_reaps=${drmd_reaps:-0}
if (( drmd_reaps < 5 )); then
  echo "Phase 1 acceptance: expected 5 clean drmd orphan reaps, got $drmd_reaps" >&2
  exit 1
fi

echo "SWAY_PHASE1_ACCEPTANCE_RESULT iterations=5 filed=[$filed_baseline] lpr=[$lpr_baseline] drmd_clean=$drmd_reaps first_frame=#242424 lifecycle=normal,term,kill"
