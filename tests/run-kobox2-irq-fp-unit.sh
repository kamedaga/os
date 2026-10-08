#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-irq-fp-unit"
mkdir -p "$out"
for cc in gcc clang; do
    for provider in native posix; do
        extra=()
        if [[ "$provider" == native ]]; then
            # Match the actual freestanding adapter's compiler boundary.
            extra+=(-DTEST_NATIVE=1 -fno-stack-protector)
        else
            extra+=("$repo_root/kobox2/linux-sandbox/kobox/host/posix/cpu.c")
        fi
        "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
            -ffunction-sections -fdata-sections -Wl,--gc-sections \
            -I "$repo_root/kobox2/linux-sandbox/kobox" \
            -I "$repo_root/userland/libpacha/include" \
            -I "$repo_root/userland/libipc/include" \
            "$repo_root/tests/kobox2_irq_fp_unit.c" \
            "$repo_root/tests/kobox2_notification_fp_invoke.S" \
            "$repo_root/kobox2/linux-sandbox/kobox/machine/domain.c" \
            "${extra[@]}" -o "$out/$provider-$cc"
        timeout 10s "$out/$provider-$cc"
    done
done
python3 "$repo_root/tests/check_kobox2_notification_fp.py" \
    "$out/native-gcc" "$out/native-clang" "$out/posix-gcc" "$out/posix-clang"
