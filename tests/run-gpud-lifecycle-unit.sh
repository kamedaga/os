#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/gpud-lifecycle-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/gpud_lifecycle_unit.c" "$repo_root/userland/gpud/lifecycle.c"
  "$repo_root/userland/gpud/process.c" "$repo_root/userland/gpud/process_native.c"
  "$repo_root/userland/kobox2_adapter/ipc.c"
  "$repo_root/kobox2/src/controller/controller.c" "$repo_root/kobox2/src/closure.c")
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/include" -I "$repo_root/kobox2/protocol/include" \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  -I "$repo_root/kobox2/protocol/generated/include" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "${sources[@]}" -o "$out/gpud-lifecycle-unit"
"$out/gpud-lifecycle-unit" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/gpud/lifecycle.h" \
  "$repo_root/userland/gpud/process.h" "$repo_root/userland/gpud/process_native.h" \
  "$repo_root/userland/kobox2_adapter/ipc.h" "$repo_root/userland/kobox2_adapter/lifecycle_message.h" \
  "$repo_root/tests/gpud_controller_fixture.h" "$repo_root/tests/run-gpud-lifecycle-unit.sh" \
  >"$out/inputs.sha256"
