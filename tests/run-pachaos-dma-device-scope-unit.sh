#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/pachaos-dma-device-scope"
mkdir -p "$out"
# Global ASan registration would retain the entire included backend ops table,
# including unrelated hardware entrypoints. Keep heap/stack instrumentation.
gcc -D_GNU_SOURCE -std=c11 -O1 -g -Wno-format \
  -fsanitize=address,undefined --param asan-globals=0 \
  -fno-omit-frame-pointer -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I "$repo_root/_kobox/include" -I "$repo_root/_kobox/src" \
  -I "$repo_root/userland/libcapsule/include" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "$repo_root/tests/pachaos_dma_device_scope_unit.c" -o "$out/test"
"$out/test" | tee "$out/result.log"
