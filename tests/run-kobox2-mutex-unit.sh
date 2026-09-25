#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-mutex-unit"
mkdir -p "$out"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread
  -fsanitize=address,undefined -fno-omit-frame-pointer
  -ffunction-sections -fdata-sections
  -I "$repo_root/kobox2/linux-sandbox/kobox"
  -I "$repo_root/userland/libpacha/include"
  -I "$repo_root/userland/libipc/include")
# Do not replace the test runner's libc memory/string routines with the
# freestanding runtime's versions (ASan interposes those symbols itself).
"${CC:-cc}" "${flags[@]}" -fno-builtin \
  -Dmemcpy=ph_test_memcpy -Dmemset=ph_test_memset -Dmemcmp=ph_test_memcmp \
  -Dstrlen=ph_test_strlen -Dstrcmp=ph_test_strcmp \
  -c "$repo_root/userland/kobox2_adapter/runtime.c" -o "$out/runtime.o"
"${CC:-cc}" "${flags[@]}" -Wl,--gc-sections \
  "$repo_root/tests/kobox2_mutex_unit.c" "$out/runtime.o" -o "$out/mutex-unit"
timeout 30s "$out/mutex-unit"
