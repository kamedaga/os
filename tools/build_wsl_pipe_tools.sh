#!/usr/bin/env bash
set -euo pipefail

mkdir -p .artifacts/userland-fixtures

if command -v musl-clang >/dev/null 2>&1; then
  cc=musl-clang
elif command -v musl-gcc >/dev/null 2>&1; then
  cc=musl-gcc
else
  echo "missing musl toolchain: install musl-tools in WSL" >&2
  exit 1
fi

"$cc" \
  -O2 \
  -fPIE \
  -pie \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  -Wl,-rpath,/lib \
  -Wl,-z,now \
  -Wl,-z,relro \
  -o .artifacts/userland-fixtures/pipe_tools.elf \
  userland/fixtures/src/wsl_musl/pipe_tools.c
