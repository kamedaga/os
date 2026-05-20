#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
output="$root_dir/.artifacts/userland-fixtures/uutils-shim.elf"
source="$root_dir/userland/fixtures/src/wsl_musl/uutils_shim.c"

mkdir -p "$(dirname "$output")"

if command -v musl-gcc >/dev/null 2>&1; then
  cc=musl-gcc
elif command -v musl-clang >/dev/null 2>&1; then
  cc=musl-clang
else
  echo "missing musl toolchain: install musl-tools in WSL" >&2
  exit 1
fi

"$cc" -Os -fPIE -pie \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  -Wl,-rpath,/lib \
  -o "$output" \
  "$source"
