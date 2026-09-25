#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/kobox-heap-index
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
    -DKOBOX_STORAGE_PROFILE=1 -ffunction-sections -fdata-sections \
    -I_kobox/include -I_kobox/src tests/kobox_heap_index_unit.c \
    -Wl,--gc-sections ${HEAP_TEST_SANITIZERS:-} \
    -o .artifacts/kobox-heap-index/unit
.artifacts/kobox-heap-index/unit
