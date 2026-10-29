#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build our Linux/musl client against pinned, unmodified Mesa and libdrm.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
output="${1:-$repo/.artifacts/amdgpu-mesa-gears-linux.elf}"
case "$output" in
    /*) ;;
    *) output="$repo/$output" ;;
esac
# The wsl_alpine_mesa recipe's outputs: the pinned runtime, its headers, and
# the record naming the Alpine build toolchain it was compiled with.
mesa_root="$repo/.artifacts/userland-fixtures/alpine-mesa-root"
mesa_include="$repo/.artifacts/userland-fixtures/alpine-mesa-dev-root/usr/include"
runtime="$mesa_root/usr/lib"
toolchain_key="$(awk '$1 == "toolchain-key" { print $2 }' "$mesa_root/usr/share/pacha/mesa-virgl-build.txt" 2>/dev/null || true)"
toolchain="${MESA_TOOLCHAIN:-$repo/.artifacts/build/mesa-25.1.9/toolchain-$toolchain_key}"
[[ -n "$toolchain_key" && -f "$toolchain/usr/bin/gcc" && -f "$mesa_include/EGL/egl.h" &&
   -f "$runtime/libEGL.so.1.0.0" && -f "$repo/tests/amdgpu_mesa_gears.c" ]] || {
    echo 'Pinned Mesa development headers/runtime are not available; build wsl_alpine_mesa first.' >&2
    exit 1
}
mkdir -p "$(dirname "$output")"
output_dir="$(dirname "$output")"
output_name="$(basename "$output")"
bwrap --unshare-user --uid 0 --gid 0 --ro-bind "$toolchain" / \
    --dev /dev --proc /proc --tmpfs /tmp \
    --ro-bind "$mesa_include/EGL" /tmp/mesa/include/EGL \
    --ro-bind "$mesa_include/GLES2" /tmp/mesa/include/GLES2 \
    --ro-bind "$mesa_include/KHR" /tmp/mesa/include/KHR \
    --ro-bind "$mesa_include/gbm.h" /tmp/mesa/include/gbm.h \
    --ro-bind "$runtime" /tmp/runtime \
    --ro-bind "$repo/tests/amdgpu_mesa_gears.c" /tmp/gears.c \
    --bind "$output_dir" /tmp/out \
    /usr/bin/gcc -std=c11 -O2 -Wall -Wextra -Werror \
    -I/tmp/mesa/include -I/usr/include/libdrm \
    /tmp/gears.c /tmp/runtime/libEGL.so.1.0.0 \
    /tmp/runtime/libGLESv2.so.2.0.0 /tmp/runtime/libgbm.so.1.0.0 \
    -ldrm -ldl -lm -Wl,-rpath-link=/usr/lib:/tmp/runtime \
    -o "/tmp/out/$output_name"
readelf -l "$output" | grep -q \
    'Requesting program interpreter: /lib/ld-musl-x86_64.so.1'
for library in libEGL.so.1 libGLESv2.so.2 libgbm.so.1 libdrm.so.2; do
    readelf -d "$output" | grep -q "Shared library: \[$library\]"
done
printf 'Built Linux/musl Mesa gears: %s\n' "$output"
