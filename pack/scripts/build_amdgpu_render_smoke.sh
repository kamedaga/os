#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build a Linux-ABI test process for the live LPR shell, not a native Pacha ELF.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output="${1:-$repo_root/.artifacts/amdgpu-render-smoke-linux.elf}"
case "$output" in
    /*) ;;
    *) output="$repo_root/$output" ;;
esac
headers="$repo_root/.artifacts/kobox2-uapi-headers"
build="$repo_root/.artifacts/kobox2-uapi-build"
cc="${PACHAOS_LINUX_MUSL_CC:-x86_64-linux-musl-gcc}"

mkdir -p "$(dirname "$output")" "$build"
# Only export headers from the pinned Linux tree. Its compiled source stays
# untouched, and the generated UAPI is isolated under .artifacts.
make -s -C "$repo_root/kobox2/linux-sandbox" \
    O="$build" ARCH=x86 INSTALL_HDR_PATH="$headers" headers_install

"$cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -I "$headers/include" \
    "$repo_root/tests/amdgpu_render_smoke.c" \
    -Wl,--dynamic-linker=/lib/linux/ld-musl-x86_64.so.1 \
    -o "$output"

readelf -l "$output" | grep -q \
    'Requesting program interpreter: /lib/linux/ld-musl-x86_64.so.1'
printf '%s\n' "$output"
