#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/gpud-launch-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/gpud_launch_unit.c"
  "$repo_root/userland/gpud/launch.c" "$repo_root/userland/gpud/process.c"
  "$repo_root/userland/kobox2_adapter/ipc.c"
  "$repo_root/kobox2/src/controller/controller.c" "$repo_root/kobox2/src/closure.c"
  "$repo_root/userland/gpud/launch_native.c" "$repo_root/userland/gpud/process_native.c"
  "$repo_root/userland/gpud/arch/x86_64/launch_image.c")
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/include" -I "$repo_root/kobox2/protocol/include" \
  -I "$repo_root/kobox2/protocol/generated/include" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "${sources[@]}" -o "$out/gpud-launch-unit"
"$out/gpud-launch-unit" "$@" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/gpud/launch_native.h" \
  "$repo_root/userland/gpud/launch.h" "$repo_root/userland/gpud/process.h" \
  "$repo_root/tests/gpud_controller_fixture.h" \
  "$repo_root/userland/gpud/launch_image.h" "$repo_root/userland/gpud/process_native.h" \
  "$repo_root/tests/run-gpud-launch-unit.sh" >"$out/inputs.sha256"
if [[ "$#" -gt 0 ]]; then sha256sum "$@" >"$out/image-inputs.sha256"; fi
