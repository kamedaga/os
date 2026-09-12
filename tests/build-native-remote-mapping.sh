#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-remote-mapping"
mkdir -p "$out"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu \
  -fuse-ld=lld -nostdlib -static-pie -fPIE -ffreestanding -fno-builtin \
  -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra -Werror \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "$repo_root/tests/native_remote_mapping.c" "$repo_root/tests/native_test_init.c" \
  "$repo_root/userland/libpacha/src/syscall.c" \
  -Wl,-e,_start,-z,noexecstack -o "$out/native_remote_mapping.elf"
