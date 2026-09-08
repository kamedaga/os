#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
clock_sysroot="$repo_root/.artifacts/third_party/alpine-lua-cli/alpine-sysroot"
clock_out="$repo_root/.artifacts/tests/lpr-linux-clock"
mkdir -p "$clock_out"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$clock_sysroot" \
  -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
  -c "$repo_root/tests/lpr_linux_clock_smoke.c" -o "$clock_out/test.o"
/usr/bin/clang -target x86_64-linux-musl --sysroot="$clock_sysroot" -nostdlib \
  "$clock_sysroot/usr/lib/Scrt1.o" "$clock_sysroot/usr/lib/crti.o" "$clock_out/test.o" \
  -L"$clock_sysroot/usr/lib" -L"$clock_sysroot/lib" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -lc \
  "$clock_sysroot/usr/lib/crtn.o" -o "$clock_out/clock-smoke"
