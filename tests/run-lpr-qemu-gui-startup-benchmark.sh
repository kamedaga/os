#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

target=${1:-gedit}
run=${GUI_STARTUP_RUN:-1}
timeout=${GUI_STARTUP_TIMEOUT:-180}
out_dir=".artifacts/test-results/gui-startup/${target}/run-${run}"
qmp_socket="$out_dir/qmp.sock"
mkdir -p "$out_dir"

case "$target" in
  sway|foot|gedit|glycin-app-png|glycin-app-png-3|glycin-app-png-small-3|glycin-png|glycin-svg) ;;
  *) printf 'unknown GUI startup target: %s\n' "$target" >&2; exit 2 ;;
esac

rm -f .artifacts/console-tty-test.log .artifacts/serial-tty-test.log \
  .artifacts/qemu-tty-host-time.log .artifacts/qemu-tty-vcpus.tsv \
  "$qmp_socket"

GUI_STARTUP_TARGET=$target \
GUI_STARTUP_TIMEOUT_SECONDS=$timeout \
GUI_STARTUP_RESULT="$out_dir/result.json" \
GUI_STARTUP_GRACEFUL="${GUI_STARTUP_GRACEFUL:-0}" \
  .artifacts/bin/pacgo qemu-test \
    --cpus 4 \
    --timeout "${timeout}s" \
    --qemu-arg=-snapshot \
    --qemu-arg=-qmp \
    --qemu-arg="unix:$qmp_socket,server=on,wait=off" \
    --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
    --python tests/qemu_lpr_gui_startup_benchmark.py

for source in \
  .artifacts/console-tty-test.log \
  .artifacts/serial-tty-test.log \
  .artifacts/qemu-tty-host-time.log; do
  if [[ -f $source ]]; then
    cp "$source" "$out_dir/${source##*/}"
  fi
done

printf 'GUI startup result: '
cat "$out_dir/result.json"
