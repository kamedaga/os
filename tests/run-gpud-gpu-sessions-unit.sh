#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/gpud-gpu-sessions-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/gpud_gpu_sessions_unit.c"
  "$repo_root/userland/gpud/gpu_sessions.c" "$repo_root/kobox2/protocol/src/gpu_session.c")
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
  "${sources[@]}" -o "$out/gpu-sessions-unit"
"$out/gpu-sessions-unit" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/gpud/gpu_sessions.h" \
  "$repo_root/kobox2/protocol/include/kobox2/gpu_session.h" \
  "$repo_root/kobox2/protocol/generated/include/kobox2/gpu_layout.h" \
  "$repo_root/tests/run-gpud-gpu-sessions-unit.sh" >"$out/inputs.sha256"
