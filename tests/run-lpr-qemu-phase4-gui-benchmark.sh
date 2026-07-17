#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

cpus="${P4_CPUS:-4}"
timeout="${P4_TIMEOUT:-240}"
case "$cpus" in
  1|4) ;;
  *) echo "P4_CPUS must be 1 or 4 for the standard comparison" >&2; exit 2 ;;
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
.artifacts/bin/pacgo qemu-test \
  --cpus "$cpus" \
  --timeout "${timeout}s" \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/phase4_gui_benchmark.sh' \
  --expect 'P4_BENCH_STARTUP phase=cold' \
  --expect 'P4_BENCH_APP app=foot' \
  --expect 'P4_BENCH_APP app=gtk3-demo' \
  --expect 'P4_BENCH_INPUT count=' \
  --expect 'P4_BENCH_FRAME source=' \
  --expect 'P4_BENCH_STARTUP phase=warm' \
  --expect 'P4_BENCH_PASS direct_sway=1 foot=1 gtk3=1 animation=1 idle=1' \
  --input-send-event \
    'P4_BENCH_FOOT_INPUT_READY key=a@key:a=down,key:a=up,key:ret=down,key:ret=up' \
  --input-send-event \
    'P4_BENCH_GTK_INPUT_READY keyboard=1 pointer=1@key:tab=down,key:tab=up,rel:x=5,rel:y=3,btn:left=down,btn:left=up' \
  --input-send-event \
    'P4_BENCH_INPUT_READY source=wayland-event-time@key:a=down,key:a=up,rel:x=7,rel:y=-4,btn:left=down,btn:left=up'

out_dir=".artifacts/phase4-step0/${cpus}cpu"
mkdir -p "$out_dir"
cp .artifacts/console-tty-test.log "$out_dir/console.log"
cp .artifacts/serial-tty-test.log "$out_dir/serial.log"
if [[ -f .artifacts/qemu-limine-host-time.log ]]; then
  cp .artifacts/qemu-limine-host-time.log "$out_dir/host-time.log"
fi

summary="$out_dir/summary.txt"
{
  printf 'phase4-step0 cpus=%s\n' "$cpus"
  rg '^P4_BENCH_(STARTUP|APP|INPUT |FRAME |IDLE_|PASS)' "$out_dir/console.log" || true
  if rg -q 'event processing lagging behind|your system is too slow' \
      "$out_dir/console.log" "$out_dir/serial.log"; then
    printf 'red libinput_event_lag=1\n'
  else
    printf 'red libinput_event_lag=0\n'
  fi
  if rg -Fq 'Broken pipe' "$out_dir/console.log" "$out_dir/serial.log"; then
    printf 'red wayland_broken_pipe=1\n'
  else
    printf 'red wayland_broken_pipe=0\n'
  fi
} | tee "$summary"
