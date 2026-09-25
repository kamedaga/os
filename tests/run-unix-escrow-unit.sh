#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/unix-escrow"
mkdir -p "$out_dir"
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/userland/unixd/include" -I"$repo_root/userland/unixd/src" \
  -I"$repo_root/userland/libipc/include" -I"$repo_root/userland/libpacha/include" \
  "$repo_root/userland/unixd/src/escrow.c" \
  "$repo_root/tests/unix_escrow_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
