#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/native-fs-fixture"
mkdir -p "$out_dir"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
  "$repo_root/tests/native_fs_fixture_unit.c" "$repo_root/kobox2/protocol/src/filesystem.c" \
  -o "$out_dir/native_fs_fixture_unit"
ASAN_OPTIONS=detect_leaks=1 "$out_dir/native_fs_fixture_unit"
