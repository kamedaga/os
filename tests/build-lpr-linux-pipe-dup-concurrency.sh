#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
sysroot="$repo_root/.artifacts/userland-fixtures/alpine-clang-root"
out="$repo_root/.artifacts/tests/lpr-linux-pipe-dup-concurrency"
mkdir -p "$out"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$sysroot" \
  -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
  -c "$repo_root/tests/lpr_linux_pipe_dup_concurrency.c" -o "$out/test.o"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$sysroot" -nostdlib \
  "$sysroot/usr/lib/Scrt1.o" "$sysroot/usr/lib/crti.o" "$out/test.o" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  "$repo_root/.artifacts/userland-fixtures/lpr-linux-musl-libc.so" \
  "$sysroot/usr/lib/crtn.o" -o "$out/pipe-dup"
