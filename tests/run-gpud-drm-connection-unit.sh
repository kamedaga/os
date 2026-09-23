#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-drm-connection
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/gpud/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  tests/gpud_drm_connection_unit.c userland/gpud/drm_files.c \
  userland/gpud/drm_control.c userland/gpud/drm_translate.c userland/gpud/drm_reply.c \
  kobox2/protocol/src/protocol.c kobox2/protocol/src/gpu.c \
  kobox2/protocol/src/gpu_session.c kobox2/protocol/src/gpu_completion.c -o "$out/unit"
"$out/unit"
