#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-process-exit-cleanup
mkdir -p "$out"
includes=(-Ikobox2/protocol/include -Ikobox2/protocol/generated/include)
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -Imusl/pachaos/include \
  tests/lpr_process_exit_cleanup_unit.c -o "$out/unit"
"$out/unit"
