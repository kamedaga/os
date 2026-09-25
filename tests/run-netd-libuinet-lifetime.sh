#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/netd-libuinet-lifetime"
archive="${1:-$repo_root/.artifacts/libuinet-pachaos/lib/libuinet.a}"
mkdir -p "$out_dir"
clang -std=c11 -g -no-pie -rdynamic -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
  -I"$repo_root/.artifacts/libuinet-pachaos/include" \
  "$repo_root/tests/netd_libuinet_lifetime.c" \
  "$archive" \
  -o "$out_dir/test"
timeout 30 "$out_dir/test"
