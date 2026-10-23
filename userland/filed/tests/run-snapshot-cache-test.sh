#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/filecache-tests
"${CC:-/usr/bin/clang}" -std=c11 -Wl,--wrap=malloc -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iuserland/filed/include -Iuserland/storage/include \
  -Iuserland/libpacha/include -Iuserland/libipc/include \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  userland/filed/tests/snapshot_cache_test.c -o .artifacts/filecache-tests/cache
.artifacts/filecache-tests/cache
