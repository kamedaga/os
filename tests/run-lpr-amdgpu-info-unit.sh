#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/tests/lpr-amdgpu-info
headers="${LIBDRM_HEADERS:-.artifacts/build/mesa-25.1.9/toolchain-b865abb753262990/usr/include/libdrm}"
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include -I"$headers" tests/lpr_amdgpu_info_unit.c -o .artifacts/tests/lpr-amdgpu-info/unit
.artifacts/tests/lpr-amdgpu-info/unit
