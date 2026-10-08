#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-supervisor-filed-session"
mkdir -p "$out_dir"
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/userland/lpr_supervisor/include" \
  -I"$repo_root/userland/libaccount/include" -I"$repo_root/userland/filed/include" \
  -I"$repo_root/userland/unixd/include" -I"$repo_root/userland/libfiled_client/include" \
  -I"$repo_root/userland/libipc/include" -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/kobox2/protocol/include" -I"$repo_root/kobox2/protocol/generated/include" \
  "$repo_root/tests/lpr_supervisor_filed_session_unit.c" \
  "$repo_root/userland/libfiled_client/src/client.c" \
  "$repo_root/kobox2/protocol/src/filesystem.c" -o "$out_dir/unit"
"$out_dir/unit"
