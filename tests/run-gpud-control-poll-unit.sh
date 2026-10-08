#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-control-poll
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O2 -Wall -Wextra -Werror \
  -fsanitize=address,undefined tests/gpud_control_poll_unit.c -o "$out/unit"
"$out/unit"
