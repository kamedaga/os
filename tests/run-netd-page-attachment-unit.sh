#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/netd-page-attachment-unit"
mkdir -p "$out_dir"

clang \
  -std=c11 -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I"$repo_root/userland/netd/include" \
  -I"$repo_root/userland/netd/src" \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/_kobox/include" \
  "$repo_root/tests/netd_page_attachment_unit.c" \
  -o "$out_dir/netd_page_attachment_unit"

"$out_dir/netd_page_attachment_unit"
