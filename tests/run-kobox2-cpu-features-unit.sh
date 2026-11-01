#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox2-cpu-features
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/kobox2_cpu_features_unit.c -o "$out/unit"
"$out/unit"
