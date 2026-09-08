#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-unix-mapping"
mkdir -p "$out_dir"
# The child deliberately faults on read-only mappings; do not create cores.
ulimit -c 0
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/userland/unixd/include" -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/libpacha/include" -I"$repo_root/musl/pachaos/include" \
  "$repo_root/userland/unixd/src/transport.c" "$repo_root/userland/unixd/src/notify.c" \
  "$repo_root/tests/lpr_unix_mapping_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
