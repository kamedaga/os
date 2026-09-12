#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-vm-execution-unit"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$repo_root/tests/kobox2_vm_execution_unit.c" \
  "$repo_root/userland/kobox2_adapter/vm_context.c" \
  "$repo_root/userland/kobox2_adapter/vm_process.c" -o "$out/vm-execution-unit"
"$out/vm-execution-unit" | tee "$out/result.log"
