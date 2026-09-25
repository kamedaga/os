#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/native-host-primitives"
mkdir -p "$out_dir"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-unknown-none-elf \
  -fuse-ld=lld -nostdlib -static -ffreestanding -fno-builtin \
  -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra -Werror \
  -I "$repo_root/userland/libpacha/include" \
  -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libcapsule/include" \
  "$repo_root/tests/native_device_contract.c" \
  -Wl,-e,_start,--image-base=0x400000 -o "$out_dir/native_device_contract.elf"
