#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/kobox2-fault-log-unit"
mkdir -p "$out"
for cc in gcc clang; do
    "$cc" -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -DPH_SANDBOX_AMDGPU=1 \
        -I "$root/kobox2/linux-sandbox/kobox" \
        -I "$root/kobox2/protocol/include" -I "$root/kobox2/protocol/generated/include" \
        -I "$root/userland/libpacha/include" -I "$root/userland/libcapsule/include" \
        -I "$root/userland/libipc/include" -I "$root/userland/personality/include" \
        "$root/tests/kobox2_fault_log_unit.c" "$root/userland/kobox2_adapter/boot.c" \
        -Wl,--gc-sections -o "$out/$cc"
    "$out/$cc"
done
echo 'Kobox2 fault RAM log: GCC/Clang PASS'
