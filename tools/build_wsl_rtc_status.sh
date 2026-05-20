#!/usr/bin/env bash
set -euo pipefail

mkdir -p .artifacts/userland-fixtures

cc=musl-gcc
if ! command -v "$cc" >/dev/null 2>&1; then
  if command -v musl-clang >/dev/null 2>&1; then
    cc=musl-clang
  else
    echo "missing musl toolchain: install musl-tools in WSL" >&2
    exit 1
  fi
fi

"$cc" \
  -fPIE \
  -pie \
  -O2 \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  -o .artifacts/userland-fixtures/rtc_status.elf \
  userland/fixtures/src/wsl_musl/rtc_status.c
