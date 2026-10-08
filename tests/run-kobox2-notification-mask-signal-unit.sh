#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-notification-mask-signal"
mkdir -p "$out"
for cc in gcc clang; do
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$repo_root/kobox2/linux-sandbox/kobox" \
    -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
    "$repo_root/tests/kobox2_notification_mask_signal_unit.c" -o "$out/$cc"
  timeout 10s "$out/$cc"
done
