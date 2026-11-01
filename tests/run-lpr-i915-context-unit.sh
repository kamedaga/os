#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-i915-context
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -Iuserland/gpud/include -Iuserland/libpacha/include -Iuserland/libipc/include \
 -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
 tests/lpr_i915_context_unit.c -o "$out/unit"
"$out/unit"
