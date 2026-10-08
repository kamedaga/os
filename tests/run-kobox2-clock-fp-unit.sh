#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/tests/kobox2-clock-fp-unit}"
mkdir -p "$out"
for compiler in gcc clang; do
  for provider in native posix; do
    extra=()
    if [[ "$provider" == native ]]; then
      extra+=(-DTEST_NATIVE=1 -fno-stack-protector)
    else
      extra+=("$repo_root/kobox2/linux-sandbox/kobox/host/posix/cpu.c")
    fi
    "$compiler" -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -I "$repo_root/kobox2/linux-sandbox/kobox" \
      -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
      "$repo_root/tests/kobox2_clock_fp_unit.c" \
      "$repo_root/tests/kobox2_notification_fp_invoke.S" \
      "${extra[@]}" -o "$out/$provider-$compiler"
    timeout 10s "$out/$provider-$compiler"
  done
done
python3 "$repo_root/tests/check_kobox2_notification_fp.py" \
  "$out/native-gcc" "$out/native-clang" "$out/posix-gcc" "$out/posix-clang"
