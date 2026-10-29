#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-futex-wake-op
mkdir -p "$out"
clang -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iuserland/libpacha/include \
  tests/lpr_futex_wake_op_unit.c -o "$out/unit"
"$out/unit"
