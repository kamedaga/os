#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/native-fs-port"
mkdir -p "$out_dir"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -Ulinux -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I "$repo_root/userland/filed/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/storage/include" \
  -I "$repo_root/userland/personality/include" \
  -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
  "$repo_root/tests/native_fs_port_unit.c" "$repo_root/userland/filed/src/exec_payload.c" \
  "$repo_root/userland/filed/src/exec/router.c" \
  -o "$out_dir/native_fs_port_unit"
ASAN_OPTIONS=detect_leaks=1 "$out_dir/native_fs_port_unit"
