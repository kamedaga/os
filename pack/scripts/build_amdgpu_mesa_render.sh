#!/usr/bin/env bash
# Build only our client against the pinned, unmodified Mesa/libdrm ABI.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
# The wsl_alpine_mesa recipe's outputs: the pinned runtime, its headers, and
# the record naming the Alpine build toolchain it was compiled with.
mesa_root="$repo/.artifacts/userland-fixtures/alpine-mesa-root"
mesa_include="$repo/.artifacts/userland-fixtures/alpine-mesa-dev-root/usr/include"
runtime="$mesa_root/usr/lib"
toolchain_key="$(awk '$1 == "toolchain-key" { print $2 }' "$mesa_root/usr/share/pacha/mesa-virgl-build.txt" 2>/dev/null || true)"
toolchain="${MESA_TOOLCHAIN:-$repo/.artifacts/build/mesa-25.1.9/toolchain-$toolchain_key}"
[[ -n "$toolchain_key" && -f "$toolchain/usr/bin/gcc" && -f "$mesa_include/EGL/egl.h" ]] || {
  echo 'Pinned Mesa development headers/runtime are not available; build wsl_alpine_mesa first.' >&2
  exit 1
}
mkdir -p "$repo/.artifacts/mesa-render-client"
bwrap --unshare-user --uid 0 --gid 0 --ro-bind "$toolchain" / \
  --dev /dev --proc /proc --tmpfs /tmp \
  --ro-bind "$mesa_include/EGL" /tmp/mesa/include/EGL \
  --ro-bind "$mesa_include/GLES2" /tmp/mesa/include/GLES2 \
  --ro-bind "$mesa_include/KHR" /tmp/mesa/include/KHR \
  --ro-bind "$mesa_include/gbm.h" /tmp/mesa/include/gbm.h \
  --ro-bind "$runtime" /tmp/runtime \
  --ro-bind "$repo/tests/amdgpu_mesa_render.c" /tmp/client.c \
  --bind "$repo/.artifacts/mesa-render-client" /tmp/out \
  /usr/bin/gcc -std=c11 -O2 -Wall -Wextra -Werror \
  -I/tmp/mesa/include -I/usr/include/libdrm \
  /tmp/client.c /tmp/runtime/libEGL.so.1.0.0 /tmp/runtime/libGLESv2.so.2.0.0 \
  /tmp/runtime/libgbm.so.1.0.0 -ldrm -ldl -Wl,-rpath-link=/usr/lib:/tmp/runtime \
  -o /tmp/out/amdgpu-mesa-render.elf
cp "$repo/.artifacts/mesa-render-client/amdgpu-mesa-render.elf" \
   "$repo/.artifacts/kobox2-amdgpu-mesa-render.elf"
readelf -l "$repo/.artifacts/kobox2-amdgpu-mesa-render.elf" | \
  grep 'Requesting program interpreter'
