#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/filed-tests
includes=()
for part in filed koboxd termd gpud inputd lpr_supervisor libipc libpacha personality; do
    includes+=("-Iuserland/$part/include")
done
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    "${includes[@]}" -Iuserland/filed/src -I_kobox/include -I_kobox/src \
    userland/filed/src/vfs/core.c userland/filed/src/vfs/object.c \
    userland/filed/src/dispatch/ops_common.c userland/filed/tests/close_reclaim_test.c \
    -o .artifacts/filed-tests/close_reclaim_test
.artifacts/filed-tests/close_reclaim_test
