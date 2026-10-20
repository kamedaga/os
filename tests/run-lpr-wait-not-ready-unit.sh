#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
out=".artifacts/tests/lpr_wait_not_ready_unit"
mkdir -p "$(dirname "$out")"
/usr/bin/clang -std=c11 -O2 -ffunction-sections -fdata-sections \
  -Iuserland/personality/include -Imusl/pachaos/include \
  -Iuserland/libpacha/include -Iuserland/libipc/include \
  -Iuserland/filed/include -Iuserland/lpr_supervisor/include \
  -Iuserland/netd/include -Iuserland/unixd/include \
  -Iuserland/termd/include -Iuserland/gpud/include -Iuserland/inputd/include \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  tests/lpr_wait_not_ready_unit.c userland/libipc/src/status.c -Wl,--gc-sections -o "$out"
"$out"
