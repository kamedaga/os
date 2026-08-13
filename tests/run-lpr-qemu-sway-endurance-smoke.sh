#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
iterations=${M58_ITERATIONS:-10}
if [[ ! "$iterations" =~ ^[0-9]+$ ]] || (( iterations < 1 || iterations > 20 )); then
  echo "sway endurance: M58_ITERATIONS must be 1..20, got [$iterations]" >&2
  exit 2
fi

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  rm -f .artifacts/userland-fixtures/lpr_sway_launcher.elf
  rm -f .artifacts/userland-fixtures/lpr_wayland_shm_client.elf
  rm -f .artifacts/pack/build/lpr_sway_launcher.sha256
  rm -f .artifacts/pack/build/lpr_wayland_shm_client.sha256
  rm -f .artifacts/pack/build/sway_endurance_script.sha256
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

args=(
  --timeout "${M58_TIMEOUT:-900s}"
  --boot-marker '[termd] linux tty hvc open ready index=0 handle='
  --send "M58_ITERATIONS=$iterations bash /cmd/sway_endurance.sh"
  --expect 'Atomic modesetting unsupported, using legacy DRM interface'
  --expect 'Creating GLES2 renderer'
  --expect 'GL renderer: llvmpipe'
  --expect 'M58_INPUT_READY iteration=1 seat=seat0 keyboard=1 pointer=1'
  --expect 'M58_INPUT_PASS iteration=1 key=30/1/0 motion=7,-4 button=272/1/0'
  --screendump-device pachagpu
  --input-send-event 'M58_INPUT_READY iteration=1 seat=seat0 keyboard=1 pointer=1@key:a=down,key:a=up,rel:x=7,rel:y=-4,btn:left=down,btn:left=up'
)
expected_escalations=0
for ((i = 1; i <= iterations; i++)); do
  case "$i" in
    2|7)
      mode=term
      sway_exit="kind=exit status=137 escalated=1"
      expected_escalations=$((expected_escalations + 1))
      ;;
    4|9)
      mode=kill
      sway_exit="kind=exit status=137 escalated=0"
      ;;
    *)
      mode=normal
      sway_exit="kind=exit status=0 escalated=0"
      ;;
  esac
  args+=(
    --expect "M58_WL_SURFACE_READY iteration=$i color=#336699 size=256x192"
    --expect "M58_CLIENT_EXIT iteration=$i status=0"
    --expect "M58_NETD_CHECKPOINT iteration=$i"
    --expect "M58_SWAY_EXIT iteration=$i mode=$mode $sway_exit"
    --expect "M56_LIFECYCLE_CLEAN mode=$mode orphan=0 stale=0 waitpid=1 iteration=$i"
    --expect "M58_ITERATION_PASS iteration=$i mode=$mode orphan=0 stale=0"
    --screendump-check "M58_WL_SURFACE_READY iteration=$i color=#336699 size=256x192@508,380,8,8=#336699"
  )
done
args+=(--expect "M58_ENDURANCE_PASS completed=$iterations")

pkill -9 qemu-system-x86 2>/dev/null || true
sleep 1
.artifacts/bin/pacgo qemu-test "${args[@]}"

console=.artifacts/console-tty-test.log
serial=.artifacts/serial-tty-test.log
actual_escalations=$(rg -Fc 'M56_LIFECYCLE_ESCALATED mode=term signal=9' "$console" || true)
all_escalations=$(rg -Fc 'M56_LIFECYCLE_ESCALATED' "$console" || true)
if (( actual_escalations != expected_escalations || all_escalations != expected_escalations )); then
  echo "sway endurance: TERM escalation mismatch expected=$expected_escalations term=$actual_escalations all=$all_escalations" >&2
  exit 1
fi
if rg -Fq 'M58_STALE_SOCKET' "$console" ||
   rg -Fq 'M58_LAUNCHER_FAIL' "$console" ||
   rg -Fq 'M56_LIFECYCLE_FAIL' "$console"; then
  echo 'sway endurance: guest lifecycle failure marker found' >&2
  exit 1
fi
if rg -Fq "failed to execute 'swaybg'" "$console" || rg -Fq 'Xwayland' "$console"; then
  echo 'sway endurance: forbidden Sway helper warning found' >&2
  exit 1
fi

mapfile -t filed_samples < <(
  sed -n 's/.*\[filed\] state_checkpoint source=client_sync active_handles=\([0-9][0-9]*\) active_sessions=\([0-9][0-9]*\).*/\1 \2/p' "$serial" | tail -n "$iterations"
)
if (( ${#filed_samples[@]} != iterations )); then
  echo "sway endurance: expected $iterations filed state samples, got ${#filed_samples[@]}" >&2
  exit 1
fi
read -r baseline_handles baseline_sessions <<<"${filed_samples[0]}"
expected_handles=$baseline_handles
# A killed GLES2/wl_shm Sway leaves one fixed four-handle signature:
# .memfd-15, .memfd-17 (transferred client backing), wlroots-AAAAAA,
# and wayland-1.lock.  Graceful rounds must remain flat; accepting any other
# per-round delta would hide a new ownership leak.
for ((i = 1; i <= iterations; i++)); do
  read -r actual_handles actual_sessions <<<"${filed_samples[$((i - 1))]}"
  if (( i == 2 || i == 4 || i == 7 || i == 9 )); then
    expected_handles=$((expected_handles + 4))
  fi
  if (( actual_handles != expected_handles || actual_sessions != baseline_sessions )); then
    echo "sway endurance: filed oracle failed iteration=$i expected=[$expected_handles $baseline_sessions] actual=[$actual_handles $actual_sessions] formula=baseline+4*(TERM_or_KILL_rounds)" >&2
    exit 1
  fi
done

for ((i = 1; i <= iterations; i++)); do
  sample=$(sed -n "s/.*\[netd\] unix_close path=\/run\/m58-netd-checkpoint-$i active=\([0-9][0-9]*\) refs=\([0-9][0-9]*\).*/\1 \2/p" "$serial")
  if [[ "$sample" != "0 0" ]]; then
    echo "sway endurance: netd state leak at iteration=$i expected=[0 0] actual=[$sample]" >&2
    exit 1
  fi
done

echo "M58_ENDURANCE_RESULT iterations=$iterations filed_baseline=[$baseline_handles $baseline_sessions] filed_final=[$expected_handles $baseline_sessions] filed_formula=baseline+4*(TERM_or_KILL_rounds) netd=[0 0] screendumps=$iterations input=pass lifecycle=normal,term-escalated,kill"
