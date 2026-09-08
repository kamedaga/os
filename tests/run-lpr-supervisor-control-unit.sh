#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-supervisor-control"
mkdir -p "$out_dir"
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/userland/lpr_supervisor/include" -I"$repo_root/userland/unixd/include" \
  -I"$repo_root/userland/libipc/include" -I"$repo_root/userland/libpacha/include" \
  "$repo_root/tests/lpr_supervisor_control_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
