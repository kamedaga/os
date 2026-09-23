#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=".artifacts/tests/kobox-rb-postorder/${CC:-cc}"
mkdir -p "$out"
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -I_kobox/include -I_kobox/src \
    tests/kobox_rb_postorder_unit.c _kobox/src/linux_personality/linux_stubs.c \
    -Wl,--gc-sections -o "$out/unit"
"$out/unit"
