#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/filed-tests
"${CC:-clang}" -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  -Iuserland/filed/include userland/filed/src/vfs/core.c userland/filed/src/vfs/object.c \
  tests/filed_vnode_growth_unit.c -Wl,--wrap=calloc,--wrap=realloc \
  -o .artifacts/filed-tests/vnode_growth_test
.artifacts/filed-tests/vnode_growth_test
