#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo_root"
out=".artifacts/fs-cutover-filed-tests/storage-wait-${CC:-clang}"
mkdir -p "$out"
"${CC:-clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/filed/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/libcapsule/include \
  -Iuserland/personality/include -Iuserland/storage/include \
  userland/filed/tests/storage_wait_test.c userland/kobox2_adapter/ipc.c \
  -o "$out/storage-wait-test"
timeout 20 "$out/storage-wait-test"
