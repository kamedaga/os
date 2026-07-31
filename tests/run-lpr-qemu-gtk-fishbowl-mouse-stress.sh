#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

fullscreen="${GTK_FISHBOWL_FULLSCREEN:-0}"
cpus="${GTK_FISHBOWL_CPUS:-4}"
mouse_hz="${GTK_FISHBOWL_MOUSE_HZ:-1000}"
mouse_delta="${GTK_FISHBOWL_MOUSE_DELTA:-1}"
case "$fullscreen" in
  0|1) ;;
  *) echo "GTK_FISHBOWL_FULLSCREEN must be 0 or 1" >&2; exit 2 ;;
esac
case "$cpus" in
  1|4) ;;
  *) echo "GTK_FISHBOWL_CPUS must be 1 or 4" >&2; exit 2 ;;
esac
case "$mouse_hz" in
  125|500|1000) ;;
  *) echo "GTK_FISHBOWL_MOUSE_HZ must be 125, 500, or 1000" >&2; exit 2 ;;
esac
case "$mouse_delta" in
  0|1) ;;
  *) echo "GTK_FISHBOWL_MOUSE_DELTA must be 0 or 1" >&2; exit 2 ;;
esac

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
  .artifacts/bin/pacgo sync bootfs
fi

out_dir=".artifacts/gtk-fishbowl-mouse-stress/${cpus}cpu-$(
  [[ $fullscreen == 1 ]] && printf fullscreen || printf windowed
)-${mouse_hz}hz"
out_dir="${out_dir}-delta${mouse_delta}"
mkdir -p "$out_dir"
trace_path="$repo_root/$out_dir/qemu-gpu.trace"
trace_events="$repo_root/tests/fixtures/phase4_gtk_fishbowl_qemu_trace_events"
rm -f "$trace_path"

stop_qemu() {
  pkill -TERM qemu-system-x86 2>/dev/null || true
}
trap stop_qemu EXIT
stop_qemu

input_hooks=(
  --input-send-event "GTK_FISHBOWL_BASELINE_BEGIN@rel:x=37"
  --input-send-event "GTK_FISHBOWL_BASELINE_END@rel:x=-37"
  --input-send-event "GTK_FISHBOWL_MOUSE_BEGIN@rel:y=-37"
)
segment_repeat=$((mouse_hz / 5))
interval_us=$((1000000 / mouse_hz))
for segment in $(seq 1 40); do
  if (( segment % 2 == 1 )); then
    delta=$mouse_delta
  else
    delta=$((-mouse_delta))
  fi
  input_hooks+=(
    --input-send-event
    "GTK_FISHBOWL_MOUSE_BEGIN@meta:repeat=${segment_repeat},meta:interval-us=${interval_us},rel:x=${delta},rel:y=${delta}"
  )
done
input_hooks+=(
  --input-send-event "GTK_FISHBOWL_MOUSE_END@rel:y=37"
)

.artifacts/bin/pacgo qemu-test \
  --cpus "$cpus" \
  --timeout 150s \
  --qemu-arg=-trace \
  --qemu-arg="events=$trace_events,file=$trace_path" \
  --send "GTK_FISHBOWL_FULLSCREEN=$fullscreen GTK_FISHBOWL_MOUSE_HZ=$mouse_hz GTK_FISHBOWL_MOUSE_DELTA=$mouse_delta bash /cmd/phase4_gtk_fishbowl.sh" \
  --expect 'GTK_FISHBOWL_SWAY_READY display=' \
  --expect 'GTK_FISHBOWL_BASELINE_BEGIN' \
  --expect 'GTK_FISHBOWL_BASELINE_END' \
  --expect "GTK_FISHBOWL_MOUSE_BEGIN nominal_hz=$mouse_hz delta=$mouse_delta" \
  --expect 'GTK_FISHBOWL_MOUSE_END' \
  --expect "GTK_FISHBOWL_PASS baseline_seconds=8 mouse_seconds=10 mouse_hz=$mouse_hz delta=$mouse_delta" \
  "${input_hooks[@]}"

cp .artifacts/console-tty-test.log "$out_dir/console.log"
cp .artifacts/serial-tty-test.log "$out_dir/serial.log"
cp .artifacts/qemu-tty-host-time.log "$out_dir/host-time.log"

read -r baseline_frames mouse_frames < <(
  awk '
    /input_event_rel/ && /axis x, value 37$/ {
      phase = "baseline"
      next
    }
    /input_event_rel/ && /axis x, value -37$/ {
      phase = ""
      next
    }
    /input_event_rel/ && /axis y, value -37$/ {
      phase = "mouse"
      next
    }
    /input_event_rel/ && /axis y, value 37$/ {
      phase = ""
      next
    }
    /virtio_gpu_cmd_res_flush/ {
      if (phase == "baseline") baseline++
      if (phase == "mouse") mouse++
    }
    END {
      printf "%d %d\n", baseline, mouse
    }
  ' "$trace_path"
)
baseline_fps=$(awk -v frames="$baseline_frames" 'BEGIN { printf "%.3f", frames / 8 }')
mouse_fps=$(awk -v frames="$mouse_frames" 'BEGIN { printf "%.3f", frames / 10 }')
ratio=$(awk -v baseline="$baseline_frames" -v mouse="$mouse_frames" \
  'BEGIN { if (baseline == 0) printf "0.000"; else printf "%.3f", (mouse / 10) / (baseline / 8) }')
{
  printf 'phase\tframes\tseconds\tfps\n'
  printf 'baseline\t%d\t8\t%s\n' "$baseline_frames" "$baseline_fps"
  printf 'mouse\t%d\t10\t%s\n' "$mouse_frames" "$mouse_fps"
  printf 'mouse_to_baseline_ratio\t-\t-\t%s\n' "$ratio"
} >"$out_dir/result.tsv"

printf 'gtk-fishbowl mouse stress completed; baseline_fps=%s mouse_fps=%s ratio=%s gpu_trace=%s\n' \
  "$baseline_fps" "$mouse_fps" "$ratio" "$trace_path"
