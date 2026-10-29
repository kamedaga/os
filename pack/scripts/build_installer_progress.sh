#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output="${1:-$repo_root/.artifacts/installer-tools/sbin/pacha-install-progress}"
mkdir -p "$(dirname "$output")"
cc="${PACHAOS_LINUX_MUSL_CC:-/usr/bin/musl-gcc}"
"$cc" -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -static \
    -I"$repo_root/userland/installer" \
    -I"$repo_root/userland/libpacha/include" -I"$repo_root/userland/libipc/include" \
    -I"$repo_root/musl/pachaos/include" \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    "$repo_root/userland/installer/linux_progress.c" \
    "$repo_root/userland/kobox2_owner/bootfs.c" -o "$output"
readelf -h "$output" | rg -q 'Machine:.*Advanced Micro Devices X86-64'
readelf -l "$output" | rg -qv 'Requesting program interpreter'
printf '%s\n' "$output"
