#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/filed-tests
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iuserland/filed/include -Iuserland/libipc/include -Iuserland/libpacha/include \
    tests/filed_vmo_fd_growth_unit.c -o .artifacts/filed-tests/vmo_fd_growth_test
.artifacts/filed-tests/vmo_fd_growth_test
