#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sysroot="$PWD/.artifacts/userland-fixtures/alpine-clang-root"
out="$PWD/.artifacts/tests/lpr-linux-gpu-mapping-race"
mkdir -p "$out"
clang -target x86_64-linux-musl --sysroot="$sysroot" -std=c11 -O2 -Wall -Wextra -Werror \
    -fPIC -c tests/lpr_linux_gpu_mapping_race.c -o "$out/test.o"
clang -target x86_64-linux-musl --sysroot="$sysroot" -nostdlib \
    "$sysroot/usr/lib/Scrt1.o" "$sysroot/usr/lib/crti.o" "$out/test.o" \
    -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 .artifacts/userland-fixtures/lpr-linux-musl-libc.so \
    "$sysroot/usr/lib/crtn.o" -o "$out/gpu-mapping-race"
