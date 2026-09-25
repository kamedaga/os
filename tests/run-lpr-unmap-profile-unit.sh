#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/lpr-unmap-profile"
mkdir -p "$out"
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
    -DLPR_UNMAP_PROFILE=1 -fsanitize=address,undefined \
    "$repo_root/tests/lpr_unmap_profile_unit.c" -o "$out/unit"
"$out/unit"
