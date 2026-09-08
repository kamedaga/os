#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/netd-fd-budget-unit"
mkdir -p "$out_dir"
clang -std=c11 -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I"$repo_root/userland/netd/include" -I"$repo_root/userland/netd/src" \
  -I"$repo_root/userland/libipc/include" -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/_kobox/include" \
  "$repo_root/tests/netd_fd_budget_unit.c" \
  "$repo_root/userland/netd/src/unix_socket.c" \
  "$repo_root/userland/netd/src/netlink_socket.c" \
  -o "$out_dir/netd_fd_budget_unit"
"$out_dir/netd_fd_budget_unit"
