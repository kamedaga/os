#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/kobox2-owner-bootfs-unit}"
mkdir -p "$out"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$repo_root/userland/libipc/include" -I"$repo_root/userland/libpacha/include" \
    "$repo_root/tests/kobox2_owner_bootfs_unit.c" \
    "$repo_root/userland/kobox2_owner/bootfs.c" -o "$out/bootfs-unit"
"$out/bootfs-unit"
