#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-credentials"
mkdir -p "$out_dir"
includes=()
for library in lpr_supervisor libipc libpacha personality unixd filed netd termd gpud inputd; do
  includes+=("-I$repo_root/userland/$library/include")
done
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
  "$repo_root/tests/lpr_credentials_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
