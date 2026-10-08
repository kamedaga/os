#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-drm-stack
mkdir -p "$out"
definitions=(-DGPUD_DRM_STACK_BUDGET=1)
if [[ -n ${GPUD_DRM_STACK_SOURCE:-} ]]; then
  definitions+=("-DGPUD_DRM_SERVICE_SOURCE=\"$GPUD_DRM_STACK_SOURCE\"")
fi
# Match the native release optimization. ASan changes the stack layout and
# needs larger thread stacks, so the separate connection test owns sanitizers.
"${CC:-clang}" -std=c11 -Oz -g -Wall -Wextra -Werror -Wno-unused-function \
  "${definitions[@]}" \
  -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/gpud -Iuserland/gpud/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  tests/gpud_drm_connection_unit.c userland/gpud/drm_files.c \
  userland/gpud/drm_control.c userland/gpud/drm_translate.c userland/gpud/drm_reply.c \
  kobox2/protocol/src/protocol.c kobox2/protocol/src/gpu.c \
  kobox2/protocol/src/gpu_session.c kobox2/protocol/src/gpu_completion.c -o "$out/unit"
"$out/unit"
