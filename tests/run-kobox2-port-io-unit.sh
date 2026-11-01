#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox2-port-io
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/kobox2_port_io_unit.c -o "$out/unit"
"$out/unit"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I userland/libpacha/include -I userland/libipc/include \
    -I userland/libcapsule/include -I kobox2/linux-sandbox/kobox \
    tests/kobox2_port_io_lifetime_unit.c -o "$out/lifetime-unit"
"$out/lifetime-unit"
