#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-bootstrap-diagnostic
mkdir -p "$out"
includes=(-Ikobox2/protocol/include -Ikobox2/protocol/generated/include)
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
for diagnostics in 0 1; do
  "${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
    -DLPR_BOOT_DIAGNOSTICS="$diagnostics" \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${includes[@]}" -Imusl/pachaos/include \
    tests/lpr_bootstrap_diagnostic_unit.c -o "$out/unit-$diagnostics"
  "$out/unit-$diagnostics"
done
