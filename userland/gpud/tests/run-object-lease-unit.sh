#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=.artifacts/tests/gpud-object-lease
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
  -D_GNU_SOURCE -Wno-unused-function -Wno-unused-variable -Wno-macro-redefined \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/gpud/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  userland/gpud/tests/object_lease_unit.c userland/gpud/ready.c \
  userland/gpud/drm_files.c -o "$out/unit"
UBSAN_OPTIONS=halt_on_error=1 "$out/unit"
