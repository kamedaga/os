#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${PRESSURE_TEST_OUT:-$root/.artifacts/vmo-memory-plan/stage5/unit}
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
 -fsanitize=address,undefined \
 -I "$root/userland/libpacha/include" -I "$root/userland/libipc/include" \
 -I "$root/kobox2/linux-sandbox/kobox" -I "$root/kobox2/protocol/include" \
 -I "$root/kobox2/protocol/generated/include" \
 "$root/userland/kobox2_adapter/tests/pressure_endpoint_unit.c" \
 -Wl,--gc-sections -o "$out/pressure-endpoint-unit"
ASAN_OPTIONS=detect_leaks=0 "$out/pressure-endpoint-unit"
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I "$root/userland/libpacha/include" -I "$root/userland/libipc/include" \
 "$root/userland/kobox2_adapter/tests/pressure_owner_unit.c" -o "$out/pressure-owner-unit"
ASAN_OPTIONS=detect_leaks=0 "$out/pressure-owner-unit"
