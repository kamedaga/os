#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/tests/kobox2-fiber-native}"
mkdir -p "$out"
adapter="$repo_root/userland/kobox2_adapter"
sources=("$repo_root/tests/kobox2_fiber_native.c" "$repo_root/tests/kobox2_notification_fp_invoke.S"
  "$adapter/runtime.c" "$adapter/native.c" "$adapter/exception.c" "$adapter/fiber.S"
  "$adapter/entry.S" "$repo_root/userland/libpacha/src/syscall.c")
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
  -nostdlib -static-pie -fPIE -ffreestanding -fno-builtin -fno-stack-protector \
  -mno-red-zone -std=gnu11 -O2 -g -Wall -Wextra -Werror \
  -I "$repo_root/kobox2/linux-sandbox/kobox" -I "$repo_root/userland/libpacha/include" \
  -I "$repo_root/userland/libipc/include" "${sources[@]}" \
  -Wl,-e,_start,-z,noexecstack -o "$out/init.elf"
sha256sum "${sources[@]}" "$adapter/host.h" "$adapter/stack_layout.h" \
  "$out/init.elf" > "$out/inputs.sha256"
