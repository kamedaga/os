#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/.artifacts/filed-wait-growth-unit"
mkdir -p "$build_dir"
includes=()
for module in filed koboxd termd gpud inputd lpr_supervisor libipc libpacha personality storage; do
    includes+=("-I$repo_root/userland/$module/include")
done
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -Wno-enum-compare -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections,--wrap=malloc \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${includes[@]}" -I"$repo_root/_kobox/include" -I"$repo_root/_kobox/src" \
    -I"$repo_root/userland/koboxd/src" \
    "$repo_root/tests/filed_wait_growth_unit.c" -o "$build_dir/test"
"$build_dir/test"
