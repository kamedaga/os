#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/fs-cutover-filed-tests
"${CC:-clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/filed/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/personality/include -Iuserland/storage/include \
  userland/filed/tests/session_detach_test.c userland/filed/src/exec_payload.c \
  userland/kobox2_owner/fs_channel.c kobox2/protocol/src/filesystem.c \
  kobox2/protocol/src/protocol.c kobox2/protocol/src/virtqueue.c \
  kobox2/protocol/src/virtqueue_memory.c kobox2/protocol/arch/x86_64/virtqueue_atomic.c \
  -o .artifacts/fs-cutover-filed-tests/session-detach-test
timeout 20 .artifacts/fs-cutover-filed-tests/session-detach-test
