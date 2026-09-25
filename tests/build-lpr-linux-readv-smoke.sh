#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
sysroot="$repo_root/.artifacts/userland-fixtures/alpine-clang-root"
runtime_libc="$repo_root/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
out="$repo_root/.artifacts/tests/lpr-linux-readv"
mkdir -p "$out"
[[ -f "$sysroot/usr/lib/Scrt1.o" ]]
/usr/bin/clang -target x86_64-linux-musl --sysroot="$sysroot" \
  -std=c11 -O2 -Wall -Wextra -Werror -fPIC -pthread \
  -c "$repo_root/tests/lpr_linux_readv_smoke.c" -o "$out/test.o"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$sysroot" -nostdlib \
  "$sysroot/usr/lib/Scrt1.o" "$sysroot/usr/lib/crti.o" "$out/test.o" \
  -L"$sysroot/usr/lib" -L"$sysroot/lib" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 "$runtime_libc" \
  "$sysroot/usr/lib/crtn.o" -o "$out/readv-smoke"
printf '%s\n' "$out/readv-smoke"
