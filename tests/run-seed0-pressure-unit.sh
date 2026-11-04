#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out="$repo/.artifacts/vmo-memory-plan/stage4a"
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O2 -Wall -Wextra -Werror \
 -I "$repo/userland/libpacha/include" -I "$repo/userland/libipc/include" \
 -I "$repo/userland/filed/include" \
 "$repo/tests/seed0_pressure_unit.c" -o "$out/seed0-pressure-unit"
"$out/seed0-pressure-unit"
"${CC:-clang}" -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
 -I "$repo/userland/libpacha/include" -I "$repo/userland/libipc/include" \
 -I "$repo/kobox2/linux-sandbox/kobox" -I "$repo/kobox2/protocol/include" \
 -I "$repo/kobox2/protocol/generated/include" \
 "$repo/tests/kobox2_pressure_unit.c" -Wl,--gc-sections -o "$out/kobox2-pressure-unit"
"$out/kobox2-pressure-unit"
