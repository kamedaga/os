#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo/.artifacts/vmo-memory-plan/stage3"
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -I "$repo/userland/kobox2_adapter" -I "$repo/userland/libpacha/include" \
  -I "$repo/userland/libipc/include" -I "$repo/kobox2/linux-sandbox/kobox" \
  -I "$repo/kobox2/protocol/include" -I "$repo/kobox2/protocol/generated/include" \
  "$repo/tests/kobox2_page_reporting_unit.c" "$repo/userland/libipc/src/ipc.c" \
  -Wl,--gc-sections -o "$out/page-reporting-unit"
"$out/page-reporting-unit"
