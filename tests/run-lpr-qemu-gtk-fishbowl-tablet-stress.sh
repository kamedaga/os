#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

fullscreen="${GTK_FISHBOWL_FULLSCREEN:-0}"
cpus="${GTK_FISHBOWL_CPUS:-4}"
tablet_hz="${GTK_FISHBOWL_TABLET_HZ:-1000}"
case "$fullscreen" in
  0|1) ;;
  *) echo "GTK_FISHBOWL_FULLSCREEN must be 0 or 1" >&2; exit 2 ;;
esac
case "$cpus" in
  1|4) ;;
  *) echo "GTK_FISHBOWL_CPUS must be 1 or 4" >&2; exit 2 ;;
esac
case "$tablet_hz" in
  125|500|1000) ;;
  *) echo "GTK_FISHBOWL_TABLET_HZ must be 125, 500, or 1000" >&2; exit 2 ;;
esac

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
  .artifacts/bin/pacgo sync bootfs
fi

out_dir=".artifacts/gtk-fishbowl-tablet-stress/${cpus}cpu-$(
  [[ $fullscreen == 1 ]] && printf fullscreen || printf windowed
)-${tablet_hz}hz"
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
  --input-send-event "GTK_FISHBOWL_BASELINE_BEGIN@key:f13=down,key:f13=up"
  --input-send-event "GTK_FISHBOWL_BASELINE_END@key:f14=down,key:f14=up"
  --input-send-event "GTK_FISHBOWL_TABLET_BEGIN@key:f15=down,key:f15=up"
)
reports=$((tablet_hz * 10))
interval_us=$((1000000 / tablet_hz))
tablet_pattern="abs:x=16352,abs:y=16352;abs:x=16384,abs:y=16352;abs:x=16384,abs:y=16384;abs:x=16352,abs:y=16384"
input_hooks+=(
  --input-send-event
  "GTK_FISHBOWL_TABLET_BEGIN@meta:repeat=${reports},meta:interval-us=${interval_us},${tablet_pattern}"
)
input_hooks+=(
  --input-send-event "GTK_FISHBOWL_TABLET_END@key:f16=down,key:f16=up"
)

.artifacts/bin/pacgo qemu-test \
  --cpus "$cpus" \
  --timeout 150s \
  --input-profile keyboard-tablet \
  --qemu-arg=-trace \
  --qemu-arg="events=$trace_events,file=$trace_path" \
  --send "GTK_FISHBOWL_FULLSCREEN=$fullscreen GTK_FISHBOWL_TABLET_HZ=$tablet_hz bash /cmd/phase4_gtk_fishbowl.sh" \
  --expect 'GTK_FISHBOWL_SWAY_READY display=' \
  --expect 'GTK_FISHBOWL_BASELINE_BEGIN' \
  --expect 'GTK_FISHBOWL_BASELINE_END' \
  --expect "GTK_FISHBOWL_TABLET_BEGIN nominal_hz=$tablet_hz" \
  --expect 'GTK_FISHBOWL_TABLET_END' \
  --expect "GTK_FISHBOWL_PASS baseline_seconds=8 tablet_seconds=10 tablet_hz=$tablet_hz" \
  "${input_hooks[@]}"

cp .artifacts/console-tty-test.log "$out_dir/console.log"
cp .artifacts/serial-tty-test.log "$out_dir/serial.log"
cp .artifacts/qemu-tty-host-time.log "$out_dir/host-time.log"

read -r baseline_frames tablet_frames < <(
  awk '
    /input_event_key_qcode/ && /key qcode f13, down 1$/ {
      phase = "baseline"
      next
    }
    /input_event_key_qcode/ && /key qcode f14, down 1$/ {
      phase = ""
      next
    }
    /input_event_key_qcode/ && /key qcode f15, down 1$/ {
      phase = "tablet"
      next
    }
    /input_event_key_qcode/ && /key qcode f16, down 1$/ {
      phase = ""
      next
    }
    /virtio_gpu_cmd_res_flush/ {
      if (phase == "baseline") baseline++
      if (phase == "tablet") tablet++
    }
    END {
      printf "%d %d\n", baseline, tablet
    }
  ' "$trace_path"
)
baseline_fps=$(awk -v frames="$baseline_frames" 'BEGIN { printf "%.3f", frames / 8 }')
tablet_fps=$(awk -v frames="$tablet_frames" 'BEGIN { printf "%.3f", frames / 10 }')
ratio=$(awk -v baseline="$baseline_frames" -v tablet="$tablet_frames" \
  'BEGIN { if (baseline == 0) printf "0.000"; else printf "%.3f", (tablet / 10) / (baseline / 8) }')
{
  printf 'phase\tframes\tseconds\tfps\n'
  printf 'baseline\t%d\t8\t%s\n' "$baseline_frames" "$baseline_fps"
  printf 'tablet\t%d\t10\t%s\n' "$tablet_frames" "$tablet_fps"
  printf 'tablet_to_baseline_ratio\t-\t-\t%s\n' "$ratio"
} >"$out_dir/result.tsv"

printf 'gtk-fishbowl tablet stress completed; baseline_fps=%s tablet_fps=%s ratio=%s gpu_trace=%s\n' \
  "$baseline_fps" "$tablet_fps" "$ratio" "$trace_path"
