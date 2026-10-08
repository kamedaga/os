#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build our diagnostic client/interposer, never modify the OSS dependency tree.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
toolchain="${PACHAOS_DESKTOP_TOOLCHAIN:-$repo/.artifacts/build/pine2-gtk-toolchain-b8cd1c9615669ce41e465ae5}"
output="$repo/.artifacts/live-desktop-frame-profile"
[[ -x "$toolchain/usr/bin/gcc" && -f "$toolchain/usr/include/GL/glx.h" ]] || {
    echo 'Desktop development toolchain missing; set PACHAOS_DESKTOP_TOOLCHAIN.' >&2
    exit 1
}
mkdir -p "$output"
bwrap --unshare-user --uid 0 --gid 0 --ro-bind "$toolchain" / \
    --dev /dev --proc /proc --tmpfs /tmp --ro-bind "$repo/tests" /tmp/tests \
    --bind "$output" /tmp/out /bin/sh -c '
    gcc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC \
        /tmp/tests/live_desktop_frame_trace.c -ldl -Wl,-soname,frame-trace.so \
        -o /tmp/out/frame-trace.so
    gcc -std=c11 -O2 -Wall -Wextra -Werror /tmp/tests/live_desktop_frame_profile.c \
        -lGL -lX11 -ldl -o /tmp/out/frame-profile.elf
    gcc -std=c11 -O2 -Wall -Wextra -Werror /tmp/tests/live_clock_rate.c \
        -pthread -o /tmp/out/clock-rate.elf
    gcc -std=c11 -O2 -Wall -Wextra -Werror /tmp/tests/live_desktop_frame_trace_unit.c \
        -ldl -o /tmp/out/frame-trace-unit
    LD_PRELOAD=/tmp/out/frame-trace.so /tmp/out/frame-trace-unit
    '
readelf -l "$output/frame-profile.elf" | grep -q \
    'Requesting program interpreter: /lib/ld-musl-x86_64.so.1'
for library in libGL.so.1 libX11.so.6 libc.musl-x86_64.so.1; do
    readelf -d "$output/frame-profile.elf" | grep -Fq "Shared library: [$library]"
done
mkdir -p "$output/rootfs/usr/libexec/pacha" "$output/rootfs/usr/lib/pacha"
install -m 0755 "$output/frame-profile.elf" "$output/rootfs/usr/libexec/pacha/frame-profile.elf"
install -m 0644 "$output/frame-trace.so" "$output/rootfs/usr/lib/pacha/frame-trace.so"
install -m 0755 "$output/clock-rate.elf" "$output/rootfs/usr/libexec/pacha/clock-rate.elf"
# GL/X11 become available only after the selected desktop closure is imported.
# Keep this tiny optional payload separate rather than weakening bootfs's ELF
# closure checks or copying graphics libraries into ordinary live startup.
tar -C "$output/rootfs" -czf "$output/frame-profile.tar.gz" \
    usr/libexec/pacha/frame-profile.elf usr/lib/pacha/frame-trace.so \
    usr/libexec/pacha/clock-rate.elf
sha256sum "$output/frame-profile.elf" "$output/frame-trace.so"
