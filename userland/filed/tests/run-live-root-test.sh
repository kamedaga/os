#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
output=.artifacts/tests/filed-live-root
mkdir -p "$output"
clang -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined \
 -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -Iuserland/filed/include -Iuserland/storage/include -Iuserland/libipc/include \
 -Iuserland/libpacha/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
 userland/filed/tests/live_root_test.c userland/filed/src/live_root.c \
 userland/kobox2_owner/bootfs.c -o "$output/test"
"$output/test"
