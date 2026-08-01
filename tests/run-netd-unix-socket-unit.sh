#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/netd-unix-socket-unit"
mkdir -p "$out_dir"

clang \
  -std=c11 \
  -Wall -Wextra -Werror \
  -I"$repo_root/userland/netd/include" \
  -I"$repo_root/userland/netd/src" \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/_kobox/include" \
  "$repo_root/tests/netd_unix_socket_unit.c" \
  -o "$out_dir/netd_unix_socket_unit"

"$out_dir/netd_unix_socket_unit"
