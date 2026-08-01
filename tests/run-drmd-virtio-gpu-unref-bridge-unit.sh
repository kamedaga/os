#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests"
mkdir -p "$out_dir"

/usr/bin/clang \
  -std=c11 \
  -Wall -Wextra -Werror \
  -I"$repo_root/_kobox/include" \
  "$repo_root/tests/drmd_virtio_gpu_unref_bridge_unit.c" \
  "$repo_root/userland/drmd/src/virtio_gpu_unref_bridge.c" \
  -o "$out_dir/drmd-virtio-gpu-unref-bridge-unit"

"$out_dir/drmd-virtio-gpu-unref-bridge-unit"
