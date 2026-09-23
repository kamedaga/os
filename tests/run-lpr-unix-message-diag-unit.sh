#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-unix-message-diag"
mkdir -p "$out_dir"
includes=()
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$module/include")
done
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
  "$repo_root/tests/lpr_unix_message_diag_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
