#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="${PACHA_NATIVE_TEST_OUTPUT_DIR:-$repo_root/.artifacts/kernel-refactor-stage10/b3}"
mkdir -p "$out_dir"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
  -nostdlib -static-pie -fPIE -ffreestanding -fno-builtin -fno-stack-protector \
  -mno-red-zone -mgeneral-regs-only -O2 -Wall -Wextra -Werror \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "$repo_root/tests/native_ipc_path_bench.c" "$repo_root/tests/native_test_init.c" \
  "$repo_root/userland/libpacha/src/syscall.c" \
  -Wl,-e,_start,-z,noexecstack -o "$out_dir/native_sel4_bench.elf"
