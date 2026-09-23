#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-vmo-lifetime"
mkdir -p "$out"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-unknown-none-elf \
  -fuse-ld=lld -nostdlib -static -ffreestanding -fno-builtin \
  -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra -Werror \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "$repo_root/tests/native_vmo_lifetime.c" \
  -Wl,-e,_start,--image-base=0x400000 -o "$out/native_vmo_lifetime.elf"
