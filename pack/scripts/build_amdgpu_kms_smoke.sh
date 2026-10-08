#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The KMS test is a Linux-ABI process launched by the optional live LPR root.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output="${1:-$repo_root/.artifacts/amdgpu-kms-smoke-linux.elf}"
case "$output" in
    /*) ;;
    *) output="$repo_root/$output" ;;
esac
cc="${PACHAOS_LINUX_MUSL_CC:-x86_64-linux-musl-gcc}"
mkdir -p "$(dirname "$output")"
"$cc" -std=c11 -O2 -Wall -Wextra -Werror \
    "$repo_root/userland/fixtures/src/wsl_musl/lpr_kms_atomic_smoke.c" \
    -Wl,--dynamic-linker=/lib/linux/ld-musl-x86_64.so.1 \
    -o "$output"
readelf -l "$output" | grep -q \
    'Requesting program interpreter: /lib/linux/ld-musl-x86_64.so.1'
printf '%s\n' "$output"
