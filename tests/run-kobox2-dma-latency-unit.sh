#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/tests/kobox2-dma-latency-unit"
mkdir -p "$out"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer)
"${CC:-cc}" "${flags[@]}" -I "$root/userland/kobox2_adapter" \
 "$root/tests/kobox2_dma_latency_unit.c" -o "$out/collector"
"$out/collector"
"${CC:-cc}" "${flags[@]}" -DPH_DMA_LATENCY=1 \
 -I "$root/userland/libpacha/include" -I "$root/userland/libipc/include" \
 -I "$root/userland/libcapsule/include" -I "$root/kobox2/linux-sandbox/kobox" \
 "$root/tests/kobox2_dma_latency_integration.c" -o "$out/integration"
"$out/integration"
