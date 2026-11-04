#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-prime-owner
mkdir -p "$out"
clang -DGPUD_READY_REFERENCE_FIXTURE -std=c11 -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/gpud/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  tests/gpud_prime_owner_unit.c tests/gpud_ready_unbound.c userland/gpud/drm_files.c -o "$out/unit"
"$out/unit"
