#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-mlock
mkdir -p "$out"
includes=()
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -Iuserland -Ikobox2/protocol/include \
  -Ikobox2/protocol/generated/include -Imusl/pachaos/include \
  tests/lpr_mlock_unit.c -o "$out/unit"
"$out/unit"
