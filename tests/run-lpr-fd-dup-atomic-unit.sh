#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/lpr-fd-dup-atomic"
mkdir -p "$out"
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/table.c" \
  "$repo_root/tests/lpr_fd_dup_atomic_unit.c" -o "$out/unit"
"$out/unit"
