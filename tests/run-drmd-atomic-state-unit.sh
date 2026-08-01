#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests"
mkdir -p "$out_dir"

/usr/bin/clang -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/userland/drmd/include" \
  -I"$repo_root/userland/drmd/src" \
  "$repo_root/tests/drmd_atomic_state_unit.c" \
  "$repo_root/userland/drmd/src/drm_atomic_state.c" \
  -o "$out_dir/drmd_atomic_state_unit"
"$out_dir/drmd_atomic_state_unit"
