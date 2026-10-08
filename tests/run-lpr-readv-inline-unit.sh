#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-readv-inline"
mkdir -p "$out_dir"
includes=()
includes+=("-I$repo_root/userland")
includes+=("-I$repo_root/kobox2/protocol/include" "-I$repo_root/kobox2/protocol/generated/include")
for component in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$component/include")
done
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
  "$repo_root/tests/lpr_readv_inline_unit.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_vfs/io.c" -o "$out_dir/unit"
"$out_dir/unit"
