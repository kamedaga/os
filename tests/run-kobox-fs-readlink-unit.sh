#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/filed-tests
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -Wno-unused-function \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Iuserland/koboxd/include -Iuserland/storage/include -Iuserland/libpacha/include -Iuserland/libipc/include \
    -I_kobox/include -I_kobox/src \
    tests/kobox_fs_readlink_unit.c -o .artifacts/filed-tests/kobox_fs_readlink_unit
.artifacts/filed-tests/kobox_fs_readlink_unit
