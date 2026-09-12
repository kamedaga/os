#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-wait-budget-unit"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$repo_root/tests/kobox2_wait_budget_unit.c" -o "$out/wait-budget-unit"
"$out/wait-budget-unit" | tee "$out/result.log"
