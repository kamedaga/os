#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-fiber-origin-unit"
mkdir -p "$out"
for cc in gcc clang; do
  # No fortified longjmp: the modelled exit leaves the fiber stack on purpose.
  "$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror -U_FORTIFY_SOURCE -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$repo_root/kobox2/linux-sandbox/kobox" \
    -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
    "$repo_root/tests/kobox2_fiber_origin_unit.c" \
    "$repo_root/userland/kobox2_adapter/fiber.S" -o "$out/$cc"
  timeout 20s "$out/$cc"
done
