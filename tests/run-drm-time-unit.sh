#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/drm-time-unit
mkdir -p "$out"
"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined tests/drm_time_unit.c -o "$out/unit"
"$out/unit"
