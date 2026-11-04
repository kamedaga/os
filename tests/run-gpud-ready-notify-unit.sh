#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-ready-notify
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread \
 -fsanitize=address,undefined -Iuserland/libipc/include -Iuserland/libpacha/include \
 tests/gpud_ready_notify_unit.c -o "$out/unit"
UBSAN_OPTIONS=halt_on_error=1 "$out/unit"
