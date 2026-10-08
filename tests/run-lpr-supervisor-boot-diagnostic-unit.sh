#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-supervisor-boot-diagnostic"
mkdir -p "$out_dir"
includes=(-I"$repo_root/userland/lpr_supervisor/include"
  -I"$repo_root/userland/libaccount/include" -I"$repo_root/userland/filed/include"
  -I"$repo_root/userland/unixd/include" -I"$repo_root/userland/libfiled_client/include"
  -I"$repo_root/userland/libipc/include" -I"$repo_root/userland/libpacha/include"
  -I"$repo_root/kobox2/protocol/include" -I"$repo_root/kobox2/protocol/generated/include")
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer "${includes[@]}" \
  "$repo_root/tests/lpr_supervisor_boot_diagnostic_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
"${CC:-clang}" -std=c11 -E "${includes[@]}" "$repo_root/userland/lpr_supervisor/src/main.c" \
  -o "$out_dir/normal.i"
if rg -q 'lprs_boot_diag|\[lprs-boot\]' "$out_dir/normal.i"; then
  echo 'boot diagnostics leaked into the default-off source' >&2
  exit 1
fi
echo 'lprs boot diagnostic: default-off preprocessed source has no observer PASS'
