#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/lpr-native-fd-capacity"
mkdir -p "$out"
/usr/bin/clang -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/musl/pachaos/include" \
  "$repo_root/tests/lpr_native_fd_capacity_unit.c" -o "$out/unit"
"$out/unit"
