#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sysroot="$PWD/.artifacts/userland-fixtures/alpine-clang-root"
runtime_libc="$PWD/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
out="$PWD/.artifacts/tests/lpr-linux-ext4-readlink"
mkdir -p "$out"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$sysroot" \
    -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
    -c tests/lpr_linux_ext4_readlink.c -o "$out/test.o"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$sysroot" -nostdlib \
    "$sysroot/usr/lib/Scrt1.o" "$sysroot/usr/lib/crti.o" "$out/test.o" \
    -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 "$runtime_libc" \
    "$sysroot/usr/lib/crtn.o" -o "$out/ext4-readlink-test"
