#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-exec-fault-unit"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$repo_root/tests/kobox2_exec_fault_unit.c" -o "$out/exec-fault-unit"
"$out/exec-fault-unit" | tee "$out/result.log"
