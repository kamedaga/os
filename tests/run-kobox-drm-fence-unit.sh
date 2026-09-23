#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-fence
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined tests/kobox_drm_fence_unit.c -o "$out/unit"
"$out/unit"
