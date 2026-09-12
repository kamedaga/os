#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/gpud-drm-translate-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/gpud_drm_translate_unit.c"
  "$repo_root/userland/gpud/drm_translate.c" "$repo_root/kobox2/protocol/src/gpu.c")
# The existing native ABI headers use wide enum constants. Treat them as
# platform headers; keep strict ISO C warnings for the translator and codec.
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
  -isystem "$repo_root/userland/drmd/include" -isystem "$repo_root/userland/libipc/include" \
  -isystem "$repo_root/userland/libpacha/include" \
  "${sources[@]}" -o "$out/drm-translate-unit"
"$out/drm-translate-unit" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/gpud/drm_translate.h" \
  "$repo_root/userland/drmd/include/drmd/ipc_protocol.h" \
  "$repo_root/tests/run-gpud-drm-translate-unit.sh" >"$out/inputs.sha256"
