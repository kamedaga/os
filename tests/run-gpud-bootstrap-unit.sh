#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/gpud-bootstrap-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/gpud_bootstrap_unit.c" "$repo_root/userland/gpud/bootstrap.c"
  "$repo_root/userland/kobox2_adapter/bootstrap.c" "$repo_root/userland/kobox2_adapter/ipc.c"
  "$repo_root/kobox2/src/controller/controller.c" "$repo_root/kobox2/src/closure.c")
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/include" -I "$repo_root/kobox2/protocol/include" \
  -I "$repo_root/kobox2/protocol/generated/include" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "${sources[@]}" -o "$out/gpud-bootstrap-unit"
"$out/gpud-bootstrap-unit" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/gpud/bootstrap.h" \
  "$repo_root/userland/kobox2_adapter/bootstrap.h" "$repo_root/userland/kobox2_adapter/ipc.h" \
  "$repo_root/tests/run-gpud-bootstrap-unit.sh" >"$out/inputs.sha256"
