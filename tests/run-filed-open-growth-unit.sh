#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/filed-tests
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iuserland/filed/include userland/filed/src/vfs/core.c userland/filed/src/vfs/object.c \
  tests/filed_open_growth_unit.c -Wl,--wrap=calloc,--wrap=realloc \
  -o .artifacts/filed-tests/open_growth_test
.artifacts/filed-tests/open_growth_test
