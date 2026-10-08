#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
toolchain="${PACHAOS_DESKTOP_TOOLCHAIN:-$repo/.artifacts/build/pine2-gtk-toolchain-b8cd1c9615669ce41e465ae5}"
output="$repo/.artifacts/live-pointer-profile"
mkdir -p "$output"
bwrap --unshare-user --uid 0 --gid 0 --ro-bind "$toolchain" / \
    --dev /dev --proc /proc --tmpfs /tmp --ro-bind "$repo/tests" /tmp/tests \
    --bind "$output" /tmp/out /bin/sh -c '
    gcc -std=c11 -O2 -Wall -Wextra -Werror /tmp/tests/live_pointer_profile.c \
        -o /tmp/out/pointer-profile.elf
    '
readelf -l "$output/pointer-profile.elf" | grep -q \
    'Requesting program interpreter: /lib/ld-musl-x86_64.so.1'
sha256sum "$output/pointer-profile.elf"
