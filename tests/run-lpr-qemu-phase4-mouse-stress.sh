#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

cpus="${P4_CPUS:-4}"
timeout="${P4_TIMEOUT:-110}"
hide_cursor="${P4_HIDE_CURSOR:-0}"
case "$cpus" in
  1|4) ;;
  *) echo "P4_CPUS must be 1 or 4 for the standard comparison" >&2; exit 2 ;;
esac
case "$hide_cursor" in
  0|1) ;;
  *) echo "P4_HIDE_CURSOR must be 0 or 1" >&2; exit 2 ;;
esac

if [[ "${SKIP_SYNC:-0}" != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
  .artifacts/bin/pacgo sync bootfs
fi

stop_qemu() {
  pkill -TERM qemu-system-x86 2>/dev/null || true
}
trap stop_qemu EXIT
stop_qemu

# qemu-test consumes input hooks in declaration order. The benchmark's initial
# input must be delivered before the stress hooks, or all pointer motion is
# consumed before the measured animation begins.
input_hooks=(
  --input-send-event
  'P4_BENCH_INPUT_READY source=wayland-event-time@key:a=down'
  --input-send-event
  'P4_BENCH_INPUT_READY source=wayland-event-time@key:a=up'
  --input-send-event
  'P4_BENCH_INPUT_READY source=wayland-event-time@rel:x=7,rel:y=-4,btn:left=down'
  --input-send-event
  'P4_BENCH_INPUT_READY source=wayland-event-time@btn:left=up'
)
for step in $(seq 1 60); do
  input_hooks+=(
    --input-send-event
    "P4_MOUSE_STEP_${step}@rel:x=1,rel:y=1"
  )
done

.artifacts/bin/pacgo qemu-test \
  --cpus "$cpus" \
  --timeout "${timeout}s" \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send "P4_SHORT_GUI_ONLY=1 P4_MOUSE_STRESS=1 P4_HIDE_CURSOR=$hide_cursor bash /cmd/phase4_gui_benchmark.sh" \
  --expect 'P4_BENCH_STARTUP phase=short' \
  --expect 'P4_BENCH_FRAME source=' \
  --expect 'P4_BENCH_PASS mode=short direct_sway=1 animation=1 cleanup=1 mouse_stress=1' \
  "${input_hooks[@]}"

out_dir=".artifacts/phase4-mouse-stress/${cpus}cpu-cursor-$(
  [[ $hide_cursor == 1 ]] && printf hidden || printf visible
)"
mkdir -p "$out_dir"
cp .artifacts/console-tty-test.log "$out_dir/console.log"
cp .artifacts/serial-tty-test.log "$out_dir/serial.log"
if [[ -f .artifacts/qemu-tty-host-time.log ]]; then
  cp .artifacts/qemu-tty-host-time.log "$out_dir/host-time.log"
fi

animation_line=$(rg -n -m 1 '^P4_BENCH_ANIMATION_MODE ' "$out_dir/console.log" |
  cut -d: -f1)
first_stress_line=$(rg -n -m 1 '^P4_MOUSE_STEP_' "$out_dir/console.log" |
  cut -d: -f1)
last_stress_line=$(rg -n '^P4_MOUSE_STEP_' "$out_dir/console.log" |
  tail -n 1 | cut -d: -f1)
frame_line=$(rg -n -m 1 '^P4_BENCH_FRAME source=' "$out_dir/console.log" |
  cut -d: -f1)
if [[ -z "$animation_line" || -z "$first_stress_line" ||
      -z "$last_stress_line" || -z "$frame_line" ||
      "$last_stress_line" -le "$animation_line" ||
      "$first_stress_line" -ge "$frame_line" ]]; then
  echo "mouse stress did not overlap the measured animation" >&2
  exit 1
fi

{
  printf 'phase4-mouse-stress cpus=%s cursor_hidden=%s overlap=1\n' \
    "$cpus" "$hide_cursor"
  rg '^P4_BENCH_(STARTUP|INPUT |FRAME source=|PASS)' "$out_dir/console.log"
  if rg -q 'event processing lagging behind|your system is too slow' \
      "$out_dir/console.log" "$out_dir/serial.log"; then
    printf 'red libinput_event_lag=1\n'
  else
    printf 'red libinput_event_lag=0\n'
  fi
} | tee "$out_dir/summary.txt"
