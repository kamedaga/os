#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/native-host-primitives"
mkdir -p "$out_dir"
if [[ "${1:-}" == --init ]]; then
  "${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu \
    -fuse-ld=lld -nostdlib -static-pie -fPIE -ffreestanding -fno-builtin \
    -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra -Werror \
    -DPACHA_NATIVE_TEST_INIT -I "$repo_root/userland/libpacha/include" \
    -I "$repo_root/userland/libipc/include" \
    "$repo_root/tests/native_execution_contract.c" "$repo_root/tests/native_clock_contract.c" \
    "$repo_root/tests/native_test_init.c" "$repo_root/userland/libpacha/src/syscall.c" \
    -Wl,-e,_start,-z,noexecstack -o "$out_dir/native_host_primitives_init.elf"
  exit
fi
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-unknown-none-elf \
  -fuse-ld=lld -nostdlib -static -ffreestanding -fno-builtin \
  -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra -Werror \
  -I "$repo_root/userland/libpacha/include" \
  -I "$repo_root/userland/libipc/include" \
  "$repo_root/tests/native_execution_contract.c" \
  "$repo_root/tests/native_clock_contract.c" \
  "$repo_root/userland/libpacha/src/syscall.c" \
  -Wl,-e,_start,--image-base=0x400000 -o "$out_dir/native_host_primitives.elf"
