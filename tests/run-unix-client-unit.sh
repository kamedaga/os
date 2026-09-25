#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/unix-client"
mkdir -p "$out_dir"
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/userland/unixd/include" -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/libpacha/include" \
  "$repo_root/userland/unixd/src/client.c" \
  "$repo_root/userland/unixd/src/client_wire.c" \
  "$repo_root/userland/libipc/src/status.c" \
  "$repo_root/tests/unix_client_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
