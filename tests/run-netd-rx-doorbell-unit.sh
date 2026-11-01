#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/.artifacts/tests/netd-rx-doorbell-unit"
mkdir -p "$out"
clang -std=c11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -I"$root/userland/libpacha/include" -I"$root/userland/libipc/include" \
 -I"$root/userland/libcapsule/include" -I"$root/userland/personality/include" \
 -I"$root/kobox2/linux-sandbox/kobox" -I"$root/kobox2/protocol/include" \
 -I"$root/kobox2/protocol/generated/include" "$root/tests/netd_rx_doorbell_unit.c" -o "$out/test"
"$out/test"
