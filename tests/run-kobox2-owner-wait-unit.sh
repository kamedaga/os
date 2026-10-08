#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/kobox2-owner-wait"
mkdir -p "$out_dir"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$repo_root/tests/kobox2_owner_wait_unit.c" -o "$out_dir/owner-wait-unit"
"$out_dir/owner-wait-unit" | tee "$out_dir/result.log"
