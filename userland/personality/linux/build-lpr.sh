#!/usr/bin/env bash
set -euo pipefail

out="${1:-.artifacts/lpr-linux-x86_64.so}"
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$(dirname "$repo_root/$out")"

clang \
  -std=c11 \
  -Wall -Wextra -Werror \
  -ffreestanding \
  -fPIC \
  -fno-stack-protector \
  -fno-builtin \
  -nostdlib \
  -shared \
  -fuse-ld=lld \
  -Wl,--no-undefined \
  -Wl,-Bsymbolic \
  -Wl,-Bsymbolic-functions \
  -Wl,-soname,lpr-linux-x86_64.so \
  -I"$repo_root/userland/personality/include" \
  -I"$repo_root/musl/pachaos/include" \
  "$repo_root/userland/personality/linux/runtime/lpr_runtime.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_zpoline.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_dispatch.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_pacha_syscall.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_entry.S" \
  -o "$repo_root/$out"
