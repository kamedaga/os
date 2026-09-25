#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
adapter="$repo_root/userland/kobox2_adapter"
out="$repo_root/.artifacts/tests/kobox2-pacha-vm"
mkdir -p "$out"
"${CAPOS_FREESTANDING_CC:-clang}" -E -P -x c \
  -I "$repo_root/kobox2/linux-sandbox/kobox" -I "$adapter" \
  "$adapter/vm_client.ld" -o "$out/vm_client.ld"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
  -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector \
  -fPIE -mno-red-zone -mgeneral-regs-only -std=c11 -O2 -g -Wall -Wextra -Werror \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/personality/include" \
  -I "$repo_root/userland/personality/linux/runtime" \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$adapter/vm_client.c" "$adapter/vm_client_entry.S" \
  "$adapter/vm_fixture.c" "$adapter/vm_fixture_entry.S" \
  "$repo_root/kobox2/linux-sandbox/kobox/tests/clients/vm_program.c" \
  "$repo_root/kobox2/linux-sandbox/kobox/arch/x86_64/vm_program.S" \
  "$repo_root/userland/libpacha/src/syscall.c" \
  -Wl,-T,"$out/vm_client.ld",-z,noexecstack \
  -o "$out/vm_client.elf"
