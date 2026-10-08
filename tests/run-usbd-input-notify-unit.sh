#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/usbd-input-notify
mkdir -p "$out"
"${CC:-clang}" -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Iuserland/libpacha/include -Iuserland/libipc/include \
    -Ikobox2/linux-sandbox/kobox -Ikobox2/protocol/include \
    tests/usbd_input_notify_unit.c -o "$out/unit"
"$out/unit"
"${CC:-clang}" -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Iuserland/libpacha/include -Iuserland/libipc/include \
    -Ikobox2/linux-sandbox/kobox -Ikobox2/protocol/include \
    tests/usbd_input_bridge_unit.c -o "$out/bridge-unit"
"$out/bridge-unit"
