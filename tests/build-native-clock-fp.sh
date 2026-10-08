#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/tests/native-clock-fp}"
mkdir -p "$out"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
  -nostdlib -static-pie -fPIE -ffreestanding -fno-builtin \
  -fno-stack-protector -mno-red-zone -std=gnu11 -O2 -g -Wall -Wextra -Werror \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "$repo_root/tests/native_clock_fp.c" "$repo_root/tests/kobox2_notification_fp_invoke.S" \
  "$repo_root/tests/native_test_init.c" "$repo_root/userland/libpacha/src/syscall.c" \
  -Wl,-e,_start,-z,noexecstack -o "$out/init.elf"
