#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo_root"
out=".artifacts/fs-cutover-filed-tests/storage-client-queue-${CC:-clang}"
mkdir -p "$out"
"${CC:-clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/filed/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/libcapsule/include -Iuserland/personality/include -Iuserland/storage/include \
  userland/filed/tests/storage_client_queue_test.c userland/kobox2_adapter/ipc.c \
  userland/kobox2_owner/fs_channel.c kobox2/protocol/src/protocol.c \
  -o "$out/storage-client-queue-test"
timeout 20 "$out/storage-client-queue-test"
