#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

run=${KEY_PHASE_RUN:-1}
timeout=${KEY_PHASE_TIMEOUT:-120}
out_dir="$repo_root/.artifacts/test-results/key-launch-phase/run-$run"
qmp_socket="$out_dir/qmp.sock"
mkdir -p "$out_dir"
rm -f "$qmp_socket" "$out_dir/result.json" "$out_dir/live-console.log"

KEY_PHASE_QMP_SOCKET="$qmp_socket" \
KEY_PHASE_OUT_DIR="$out_dir" \
KEY_PHASE_RESULT="$out_dir/result.json" \
KEY_PHASE_LIVE_LOG="$out_dir/live-console.log" \
  .artifacts/bin/pacgo qemu-test \
    --cpus 4 \
    --timeout "${timeout}s" \
    --qemu-arg=-snapshot \
    --qemu-arg=-qmp \
    --qemu-arg="unix:$qmp_socket,server=on,wait=off" \
    --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
    --python tests/qemu_lpr_key_launch_phase_benchmark.py

if [[ -s $out_dir/live-console.log ]]; then
  cp "$out_dir/live-console.log" "$out_dir/console.log"
else
  cp .artifacts/console-tty-test.log "$out_dir/console.log"
fi
cp .artifacts/serial-tty-test.log "$out_dir/serial.log"
cp .artifacts/qemu-tty-host-time.log "$out_dir/host-time.log"

python3 tests/analyze_lpr_startup_profile.py \
  --serial "$out_dir/serial.log" \
  --console "$out_dir/console.log" \
  --manifest .artifacts/manifests/rootfs.generated.txt \
  --root .artifacts/third_party/alpine-input-v3.22-x86_64/root \
  --limit 20 >"$out_dir/profile.md"

printf 'KEY launch phase result: %s\n' "$out_dir/result.json"
cat "$out_dir/result.json"
