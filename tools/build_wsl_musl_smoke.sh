#!/usr/bin/env bash
set -euo pipefail

mkdir -p userland/fixtures

if command -v musl-clang >/dev/null 2>&1; then
  cc=musl-clang
elif command -v musl-gcc >/dev/null 2>&1; then
  cc=musl-gcc
else
  echo "missing musl toolchain: install musl-tools in WSL" >&2
  exit 1
fi

libc_so=
for candidate in \
  /usr/lib/x86_64-linux-musl/libc.so \
  /lib/x86_64-linux-musl/libc.so
do
  if [ -f "$candidate" ]; then
    libc_so=$candidate
    break
  fi
done

if [ -z "$libc_so" ]; then
  echo "missing musl libc.so" >&2
  exit 1
fi

cp "$libc_so" userland/fixtures/libc.so

"$cc" \
  -fPIE \
  -pie \
  -nostartfiles \
  -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/ld.so \
  -Wl,-rpath,/lib \
  -Wl,-z,now \
  -Wl,-z,relro \
  -o userland/fixtures/musl_smoke.elf \
  userland/fixtures/wsl_musl/musl_smoke.c
