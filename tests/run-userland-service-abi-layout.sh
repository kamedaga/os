#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests"
mkdir -p "$out_dir"

cc="${CC:-cc}"
"$cc" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libpacha/include" \
  -I "$repo_root/userland/filed/include" \
  -I "$repo_root/userland/koboxd/include" \
  -I "$repo_root/userland/koboxd/src" \
  -I "$repo_root/userland/netd/include" \
  -I "$repo_root/userland/termd/include" \
  -I "$repo_root/userland/lpr_supervisor/include" \
  -I "$repo_root/userland/personality/include" \
  "$repo_root/tests/userland_service_abi_layout.c" \
  -o "$out_dir/userland_service_abi_layout"

"$out_dir/userland_service_abi_layout"
