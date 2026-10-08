#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/fs-policy-unit"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/protocol/include" \
  -I "$repo_root/kobox2/protocol/generated/include" \
  "$repo_root/tests/kobox2_fs_policy_unit.c" \
  "$repo_root/kobox2/protocol/src/filesystem.c" -o "$out/policy-unit"
"$out/policy-unit"
