#!/usr/bin/env bash
# Filed/VFS microbenchmark through LPR. The same static binary also runs on
# Linux, so results are compared per phase in ns_per_op rather than end-to-end
# gate time, which is dominated by components other than filed.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

scale="${FS_BENCH_SCALE:-2}"
trials="${FS_BENCH_TRIALS:-3}"
out="${FS_BENCH_OUT:-.artifacts/fs-bench/pacha.log}"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

pkill -9 qemu-system-x86 || true
sleep 1
.artifacts/bin/pacgo qemu-test \
  --console-shell \
  --timeout "${FS_BENCH_TIMEOUT:-1200s}" \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send "sh /cmd/lpr_fs_microbench.sh $scale $trials" \
  --expect 'FS_MICROBENCH_DONE'

mkdir -p "$(dirname "$out")"
grep -a 'FS_BENCH' .artifacts/console-tty-test.log | tr -d '\r' > "$out"
if grep -q 'FS_BENCH_FAIL' "$out"; then
  grep 'FS_BENCH_FAIL' "$out" >&2
  exit 1
fi
echo "fs microbench: $(grep -c FS_BENCH_DONE "$out") runs -> $out"
