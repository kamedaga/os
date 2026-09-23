#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-drm-capacity
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/gpud/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  tests/gpud_drm_capacity_unit.c -o "$out/unit"
"$out/unit"
