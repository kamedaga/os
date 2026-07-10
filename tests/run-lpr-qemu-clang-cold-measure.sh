#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

start_seconds=$SECONDS
.artifacts/bin/pacgo qemu-test \
  --timeout "${CLANG_COLD_TIMEOUT:-600s}" \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send '. /cmd/clang_cold_measure.sh' \
  --expect 'CLANG_COLD_MEASURE_DONE'
host_elapsed=$((SECONDS - start_seconds))

guest_elapsed="$(sed -n 's/.*CLANG_COLD_MEASURE_OK elapsed_s=\([0-9][0-9]*\).*/\1/p' .artifacts/console-tty-test.log | tail -n 1)"
if [[ -z "$guest_elapsed" ]]; then
  failure="$(sed -n 's/.*CLANG_COLD_VERSION_FAIL status=\([0-9][0-9]*\).*/\1/p' .artifacts/console-tty-test.log | tail -n 1)"
  echo "clang cold measure: clang failed status=${failure:-unknown}" >&2
  exit 1
fi
echo "CLANG_COLD_RESULT guest_elapsed_s=$guest_elapsed host_elapsed_s=$host_elapsed"
