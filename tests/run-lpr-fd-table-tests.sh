#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests"
mkdir -p "$out_dir"

cc="${CC:-cc}"
"$cc" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I "$repo_root/userland/personality/linux/runtime" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/table.c" \
  "$repo_root/tests/lpr_fd_table_test.c" \
  -o "$out_dir/lpr_fd_table_test"

"$out_dir/lpr_fd_table_test"
