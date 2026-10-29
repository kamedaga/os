#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/tests/kobox2-dma-log-unit"
mkdir -p "$out"
"${CC:-cc}" -std=gnu11 -O2 -Wall -Wextra -Werror -DPH_SANDBOX_WIFI=1 \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I "$root/userland/libpacha/include" -I "$root/userland/libipc/include" \
  -I "$root/kobox2/linux-sandbox/kobox" \
  "$root/tests/kobox2_dma_log_unit.c" "$root/userland/kobox2_adapter/boot.c" \
  -o "$out/test"
"$out/test" | tee "$out/result.log"
