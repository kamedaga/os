#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/ipc-register-codec
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iuserland/libpacha/include -Iuserland/libipc/include \
  tests/ipc_register_codec_unit.c -o "$out/unit"
"$out/unit"
