#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/gpud-drm-files-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/gpud_drm_files_unit.c"
  "$repo_root/userland/gpud/drm_files.c" "$repo_root/userland/gpud/drm_control.c"
  "$repo_root/userland/gpud/drm_translate.c" "$repo_root/userland/gpud/drm_reply.c"
  "$repo_root/kobox2/protocol/src/gpu.c" "$repo_root/kobox2/protocol/src/gpu_completion.c"
  "$repo_root/kobox2/protocol/src/protocol.c" "$repo_root/kobox2/protocol/src/gpu_session.c")
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
  -isystem "$repo_root/userland/gpud/include" -isystem "$repo_root/userland/libipc/include" \
  -isystem "$repo_root/userland/libpacha/include" "${sources[@]}" -o "$out/drm-files-unit"
"$out/drm-files-unit" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/gpud/drm_files.h" \
  "$repo_root/userland/gpud/drm_control.h" "$repo_root/userland/gpud/drm_translate.h" \
  "$repo_root/userland/gpud/drm_reply.h" "$repo_root/tests/run-gpud-drm-files-unit.sh" \
  >"$out/inputs.sha256"
