#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/bootstrap-extent"
mkdir -p "$out_dir"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -I "$repo_root/userland/seed0boot/src" \
    -I "$repo_root/userland/libpacha/include" \
    "$repo_root/tests/bootstrap_extent_unit.c" -o "$out_dir/bootstrap-extent"
"$out_dir/bootstrap-extent"
