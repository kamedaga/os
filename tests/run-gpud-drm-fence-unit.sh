#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-drm-fence
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/gpud/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  tests/gpud_drm_fence_unit.c kobox2/protocol/src/protocol.c -o "$out/unit"
"$out/unit"
