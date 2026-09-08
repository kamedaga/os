#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-inotify-close-race"
mkdir -p "$out_dir"
includes=()
for component in libipc libpacha personality filed termd drmd inputd netd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$component/include")
done
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
  -I"$repo_root/userland/daemons/common/include" -I"$repo_root/_kobox/include" \
  "$repo_root/tests/lpr_inotify_close_race_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
