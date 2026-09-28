#!/usr/bin/env bash
# Build only our client against the pinned, unmodified Mesa/libdrm ABI.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
toolchain="${MESA_TOOLCHAIN:-$repo/.artifacts/build/mesa-25.1.9/toolchain-b865abb753262990}"
source_tree="$repo/.artifacts/build/mesa-25.1.9/0c2ef400f215231042faeb98f8cc9145/source"
runtime="$repo/.artifacts/kobox2-amdgpu-mesa-package/rootfs/usr/lib"
mkdir -p "$repo/.artifacts/mesa-render-client"
bwrap --unshare-user --uid 0 --gid 0 --ro-bind "$toolchain" / \
  --dev /dev --proc /proc --tmpfs /tmp \
  --ro-bind "$source_tree" /tmp/mesa \
  --ro-bind "$runtime" /tmp/runtime \
  --ro-bind "$repo/tests/amdgpu_mesa_render.c" /tmp/client.c \
  --bind "$repo/.artifacts/mesa-render-client" /tmp/out \
  /usr/bin/gcc -std=c11 -O2 -Wall -Wextra -Werror \
  -I/tmp/mesa/include -I/tmp/mesa/src/gbm/main -I/usr/include/libdrm \
  /tmp/client.c /tmp/runtime/libEGL.so.1.0.0 /tmp/runtime/libGLESv2.so.2.0.0 \
  /tmp/runtime/libgbm.so.1.0.0 -ldrm -ldl -Wl,-rpath-link=/usr/lib:/tmp/runtime \
  -o /tmp/out/amdgpu-mesa-render.elf
cp "$repo/.artifacts/mesa-render-client/amdgpu-mesa-render.elf" \
   "$repo/.artifacts/kobox2-amdgpu-mesa-render.elf"
readelf -l "$repo/.artifacts/kobox2-amdgpu-mesa-render.elf" | \
  grep 'Requesting program interpreter'
