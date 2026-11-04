#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests"
mkdir -p "$out"
includes=()
for path in userland/personality/include musl/pachaos/include userland/libpacha/include userland/libipc/include userland/filed/include userland/lpr_supervisor/include userland/netd/include userland/unixd/include userland/termd/include userland/gpud/include userland/inputd/include userland kobox2/protocol/include kobox2/protocol/generated/include; do includes+=("-I$repo_root/$path"); done
cc -O2 -g -ffunction-sections -fdata-sections "${includes[@]}" "$repo_root/userland/personality/linux/tests/input_ring_test.c" -Wl,--gc-sections -pthread -o "$out/lpr-input-ring-unit"
"$out/lpr-input-ring-unit"
