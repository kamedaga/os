#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

start_seconds=$SECONDS
iterations=5
sample_count=$((iterations + 2))
args=(
  --timeout 600s
  --boot-marker '[termd] linux tty hvc open ready index=0 handle='
  --send '. /cmd/clang_endurance_smoke.sh'
  --expect 'CLANG_VERSION_OK'
  --expect 'CLANG_OBJECT_STRESS_OK count=140'
  --expect 'CLANG_COMPILE_ONLY_OK'
  --expect 'CLANG_LINK_RUN_OK'
)
for i in $(seq 1 "$iterations"); do
  args+=(--expect "CLANG_ENDURANCE_ITER_OK iteration=$i")
done
args+=(--expect "CLANG_ENDURANCE_DONE iterations=$iterations")
.artifacts/bin/pacgo qemu-test "${args[@]}"
host_elapsed=$((SECONDS - start_seconds))

serial=.artifacts/serial-tty-test.log
console=.artifacts/console-tty-test.log

mapfile -t state_samples < <(
  awk '
    /\[filed\] state_checkpoint source=client_sync / {
      handles = sessions = ""
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^active_handles=/) { split($i, value, "="); handles = value[2] }
        if ($i ~ /^active_sessions=/) { split($i, value, "="); sessions = value[2] }
      }
      if (handles != "" && sessions != "") {
        print NR, handles, sessions
      }
    }
  ' "$serial"
)
if (( ${#state_samples[@]} < sample_count )); then
  echo "clang endurance: expected at least $sample_count Filed state samples, got ${#state_samples[@]}" >&2
  exit 1
fi
state_samples=("${state_samples[@]: -sample_count}")

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
if (( ${#kobox_samples[@]} < sample_count )); then
  echo "clang endurance: expected at least $sample_count kobox object samples, got ${#kobox_samples[@]}" >&2
  exit 1
fi
baseline_kobox="${kobox_samples[${#kobox_samples[@]} - sample_count]}"
converged_kobox="${kobox_samples[${#kobox_samples[@]} - sample_count + 1]}"
final_kobox="${kobox_samples[${#kobox_samples[@]} - 1]}"
read -r baseline_line baseline_used baseline_referenced baseline_cached baseline_evictions <<<"$baseline_kobox"
read -r _ converged_used converged_referenced converged_cached converged_evictions <<<"$converged_kobox"
read -r final_line final_used final_referenced final_cached final_evictions <<<"$final_kobox"

for ((index = ${#kobox_samples[@]} - sample_count + 1; index < ${#kobox_samples[@]} - 1; index++)); do
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

read -r _ baseline_handles baseline_sessions <<<"${state_samples[0]}"
for sample in "${state_samples[@]}"; do
  read -r _ handles sessions <<<"$sample"
  if [[ "$handles $sessions" != "$baseline_handles $baseline_sessions" ]]; then
    echo "clang endurance: Filed state mismatch baseline=[$baseline_handles $baseline_sessions] actual=[$handles $sessions]" >&2
    exit 1
  fi
done

mapfile -t iteration_seconds < <(
  sed -n 's/.*CLANG_ENDURANCE_ITER_OK iteration=[0-9][0-9]* elapsed_s=\([0-9][0-9]*\).*/\1/p' "$console"
)
if (( ${#iteration_seconds[@]} != iterations )); then
  echo "clang endurance: expected $iterations timing samples, got ${#iteration_seconds[@]}" >&2
  exit 1
fi
total_seconds=0
for elapsed in "${iteration_seconds[@]}"; do
  total_seconds=$((total_seconds + elapsed))
done

echo "CLANG_ENDURANCE_RESULT iterations=$iterations guest_elapsed_s=$total_seconds host_elapsed_s=$host_elapsed filed_handles=$baseline_handles filed_sessions=$baseline_sessions kobox_baseline=[$baseline_used $baseline_referenced $baseline_cached $baseline_evictions] kobox_converged=[$converged_used $converged_referenced $converged_cached $converged_evictions] kobox_final=[$final_used $final_referenced $final_cached $final_evictions]"
