#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-ipc-unit/${CC:-cc}"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "$repo_root/tests/kobox2_ipc_unit.c" -o "$out/ipc-unit"
"$out/ipc-unit" | tee "$out/result.log"
sha256sum "$repo_root/userland/kobox2_adapter/ipc.c" \
  "$repo_root/userland/kobox2_adapter/ipc.h" "$repo_root/tests/kobox2_ipc_unit.c" \
  "$repo_root/tests/run-kobox2-ipc-unit.sh" >"$out/inputs.sha256"
