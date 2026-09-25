#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-cpu-wait-unit"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=undefined \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  "$repo_root/tests/kobox2_cpu_wait_unit.c" \
  "$repo_root/kobox2/linux-sandbox/kobox/machine/domain.c" -o "$out/cpu-wait-unit"
timeout 10s "$out/cpu-wait-unit"
