#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-lifecycle-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/kobox2_lifecycle_unit.c" "$repo_root/userland/kobox2_adapter/lifecycle.c")
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "${sources[@]}" -o "$out/lifecycle-unit"
"$out/lifecycle-unit" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/kobox2_adapter/lifecycle.h" \
  "$repo_root/kobox2/linux-sandbox/kobox/boot/lifecycle.h" \
  "$repo_root/tests/run-kobox2-lifecycle-unit.sh" >"$out/inputs.sha256"
