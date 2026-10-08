#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/kobox2-owner-boot"
mkdir -p "$out_dir"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
    -I "$repo_root/kobox2/include" -I "$repo_root/kobox2/protocol/include" \
    -I "$repo_root/kobox2/protocol/generated/include" \
    "$repo_root/tests/kobox2_owner_boot_unit.c" \
    "$repo_root/userland/kobox2_owner/boot.c" -o "$out_dir/owner-boot-unit"
"$out_dir/owner-boot-unit" | tee "$out_dir/result.log"
