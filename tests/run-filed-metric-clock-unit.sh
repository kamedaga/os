#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/filed-tests
includes=()
for part in filed koboxd termd gpud inputd lpr_supervisor libipc libpacha personality; do
    includes+=("-Iuserland/$part/include")
done
for enabled in 0 1; do
    "${CC:-clang}" -std=c11 -D_POSIX_C_SOURCE=200809L \
        -DFILED_WALL_TIME_METRICS="$enabled" -Wall -Wextra -Werror \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        "${includes[@]}" -Iuserland/filed/src -I_kobox/include -I_kobox/src \
        tests/filed_metric_clock_unit.c \
        -o ".artifacts/filed-tests/metric-clock-$enabled"
    ".artifacts/filed-tests/metric-clock-$enabled"
done
