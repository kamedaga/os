#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/usbd-diagnostic-unit"
mkdir -p "$out"
"${CC:-clang}" -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -fsanitize=address,undefined \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/usbd/include" -I "$repo_root/userland/libfiled_client/include" \
  -I "$repo_root/userland/filed/include" -I "$repo_root/kobox2/include" \
  -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
  "$repo_root/tests/usbd_diagnostic_unit.c" -Wl,--gc-sections -o "$out/diagnostic-unit"
"$out/diagnostic-unit" | tee "$out/result.log"
