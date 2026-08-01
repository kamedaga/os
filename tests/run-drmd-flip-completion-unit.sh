#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests"
mkdir -p "$out_dir"

/usr/bin/clang \
  -std=c11 \
  -Wall -Wextra -Werror \
  "$repo_root/tests/drmd_flip_completion_unit.c" \
  -o "$out_dir/drmd-flip-completion-unit"

"$out_dir/drmd-flip-completion-unit"
