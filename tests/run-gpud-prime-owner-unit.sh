#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-prime-owner
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/gpud/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  tests/gpud_prime_owner_unit.c userland/gpud/drm_files.c -o "$out/unit"
"$out/unit"
