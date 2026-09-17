#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
statat_sysroot="$repo_root/.artifacts/userland-fixtures/alpine-clang-root"
runtime_libc="$repo_root/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
statat_out="$repo_root/.artifacts/tests/lpr-linux-statat"
mkdir -p "$statat_out"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$statat_sysroot" \
  -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
  -c "$repo_root/tests/lpr_linux_statat_smoke.c" -o "$statat_out/test.o"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$statat_sysroot" -nostdlib \
  "$statat_sysroot/usr/lib/Scrt1.o" "$statat_sysroot/usr/lib/crti.o" "$statat_out/test.o" \
  -L"$statat_sysroot/usr/lib" -L"$statat_sysroot/lib" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 "$runtime_libc" \
  "$statat_sysroot/usr/lib/crtn.o" -o "$statat_out/statat-smoke"
