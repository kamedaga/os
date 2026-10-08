#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/tests/kobox2-clock-page-unit}"
mkdir -p "$out"
for compiler in gcc clang; do
  "$compiler" -std=gnu11 -O2 -Wall -Wextra -Werror \
    -I "$repo_root/kobox2/linux-sandbox/kobox" \
    -I "$repo_root/userland/libpacha/include" \
    "$repo_root/tests/kobox2_clock_page_unit.c" -o "$out/clock-page-$compiler"
  timeout 10s "$out/clock-page-$compiler"
done
