#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
source_root="${PACHA_BENCH_SOURCE_ROOT:-$repo_root}"
out="${PACHA_NATIVE_TEST_OUTPUT_DIR:-$repo_root/.artifacts/vmo-memory-plan/stage2/bench/after}"
mkdir -p "$out"
cases=(0 1)
if rg -q '^#define PACHA_FD_SYSCALL_VMO_DISCARD ' "$source_root/userland/libpacha/include/pacha/abi.h"; then cases+=(2 3 4 5 6); fi
for case_id in "${cases[@]}"; do
  "${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu \
    -fuse-ld=lld -nostdlib -static-pie -fPIE -ffreestanding -fno-builtin \
    -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -DBENCH_CASE="$case_id" \
    -I "$source_root/userland/libpacha/include" -I "$source_root/userland/libipc/include" \
    "$repo_root/tests/native_vmo_discard_bench.c" "$repo_root/tests/native_test_init.c" \
    "$source_root/userland/libpacha/src/syscall.c" \
    -Wl,-e,_start,-z,noexecstack -o "$out/case-$case_id.elf"
done
