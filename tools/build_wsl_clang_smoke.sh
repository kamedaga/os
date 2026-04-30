#!/usr/bin/env bash
set -euo pipefail

mkdir -p userland/fixtures

clang -fuse-ld=lld \
  -target x86_64-linux-gnu \
  -fPIC \
  -shared \
  -nostdlib \
  -Wl,-soname,libsmoke_clang.so \
  -o userland/fixtures/libsmoke_clang.so \
  userland/fixtures/wsl_clang/libsmoke_clang.c

clang -fuse-ld=lld \
  -target x86_64-linux-gnu \
  -fPIE \
  -pie \
  -nostdlib \
  -Wl,--dynamic-linker=/lib/ld.so \
  -Wl,-rpath,/lib \
  -Luserland/fixtures \
  -lsmoke_clang \
  -o userland/fixtures/smoke_clang.elf \
  userland/fixtures/wsl_clang/smoke_clang.c
