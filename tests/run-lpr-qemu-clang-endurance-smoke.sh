#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

start_seconds=$SECONDS
args=(
  --timeout 600s
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2'
  --send '. /cmd/clang_endurance_smoke.sh'
  --expect 'CLANG_VERSION_OK'
  --expect 'CLANG_OBJECT_STRESS_OK count=140'
  --expect 'CLANG_COMPILE_ONLY_OK'
  --expect 'CLANG_LINK_RUN_OK'
)
for i in $(seq 1 10); do
  args+=(--expect "CLANG_ENDURANCE_ITER_OK iteration=$i")
done
args+=(--expect 'CLANG_ENDURANCE_DONE iterations=10')
.artifacts/bin/pacgo qemu-test "${args[@]}"
host_elapsed=$((SECONDS - start_seconds))

serial=.artifacts/serial-tty-test.log
console=.artifacts/console-tty-test.log
counts_hash=1788043886520475514
reason_hash=13796112191661380053

mapfile -t state_samples < <(
  awk -v event="a0=$counts_hash" -v reason="a5=$reason_hash" '
    index($0, event) && index($0, reason) {
      open_count = ""
      live_count = ""
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^a3=/) { split($i, value, "="); open_count = value[2] }
        if ($i ~ /^a4=/) { split($i, value, "="); live_count = value[2] }
      }
      if (open_count != "" && live_count != "") {
        print NR, open_count, live_count
      }
    }
  ' "$serial"
)
if (( ${#state_samples[@]} < 2 )); then
  echo "clang endurance: expected lpr.state.counts samples, got ${#state_samples[@]}" >&2
  exit 1
fi

mapfile -t kobox_samples < <(
  awk '
    /\[filed\] kobox_objects / {
      used = referenced = cached = evictions = ""
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^used=/) { split($i, v, "="); used = v[2] }
        if ($i ~ /^referenced=/) { split($i, v, "="); referenced = v[2] }
        if ($i ~ /^cached=/) { split($i, v, "="); cached = v[2] }
        if ($i ~ /^evictions=/) { split($i, v, "="); evictions = v[2] }
      }
      if (used != "" && referenced != "" && cached != "" && evictions != "") {
        print NR, used, referenced, cached, evictions
      }
    }
  ' "$serial"
)
if (( ${#kobox_samples[@]} < 12 )); then
  echo "clang endurance: expected at least 12 kobox object samples, got ${#kobox_samples[@]}" >&2
  exit 1
fi
baseline_kobox="${kobox_samples[${#kobox_samples[@]} - 12]}"
converged_kobox="${kobox_samples[${#kobox_samples[@]} - 11]}"
final_kobox="${kobox_samples[${#kobox_samples[@]} - 1]}"
read -r baseline_line baseline_used baseline_referenced baseline_cached baseline_evictions <<<"$baseline_kobox"
read -r _ converged_used converged_referenced converged_cached converged_evictions <<<"$converged_kobox"
read -r final_line final_used final_referenced final_cached final_evictions <<<"$final_kobox"

for ((index = ${#kobox_samples[@]} - 11; index < ${#kobox_samples[@]} - 1; index++)); do
  read -r _ used referenced cached evictions <<<"${kobox_samples[$index]}"
  if [[ "$used $referenced $cached $evictions" != \
        "$converged_used $converged_referenced $converged_cached $converged_evictions" ]]; then
    echo "clang endurance: kobox object table did not converge sample=[$used $referenced $cached $evictions] expected=[$converged_used $converged_referenced $converged_cached $converged_evictions]" >&2
    exit 1
  fi
done
if [[ "$final_used" != "$converged_used" || "$final_evictions" != "$converged_evictions" ||
      $((final_referenced + final_cached)) -ne final_used ]]; then
  echo "clang endurance: final kobox usage left steady state converged=[$converged_used $converged_referenced $converged_cached $converged_evictions] final=[$final_used $final_referenced $final_cached $final_evictions]" >&2
  exit 1
fi

before_state=""
after_state=""
for sample in "${state_samples[@]}"; do
  read -r line open_count live_object_count <<<"$sample"
  if (( line <= baseline_line )); then
    before_state="$open_count $live_object_count"
  fi
  if (( line <= final_line )); then
    after_state="$open_count $live_object_count"
  fi
done
if [[ -z "$before_state" || -z "$after_state" || "$before_state" != "$after_state" ]]; then
  echo "clang endurance: LPR state mismatch before=[$before_state] after=[$after_state]" >&2
  exit 1
fi

mapfile -t iteration_seconds < <(
  sed -n 's/.*CLANG_ENDURANCE_ITER_OK iteration=[0-9][0-9]* elapsed_s=\([0-9][0-9]*\).*/\1/p' "$console"
)
if (( ${#iteration_seconds[@]} != 10 )); then
  echo "clang endurance: expected 10 timing samples, got ${#iteration_seconds[@]}" >&2
  exit 1
fi
total_seconds=0
for elapsed in "${iteration_seconds[@]}"; do
  total_seconds=$((total_seconds + elapsed))
done

read -r open_count live_object_count <<<"$after_state"
echo "CLANG_ENDURANCE_RESULT iterations=10 guest_elapsed_s=$total_seconds host_elapsed_s=$host_elapsed open_count=$open_count live_object_count=$live_object_count kobox_baseline=[$baseline_used $baseline_referenced $baseline_cached $baseline_evictions] kobox_converged=[$converged_used $converged_referenced $converged_cached $converged_evictions] kobox_final=[$final_used $final_referenced $final_cached $final_evictions]"
