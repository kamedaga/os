#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out="$repo/.artifacts/gtk-frame-observer"
mkdir -p "$out/headers"
if [[ ! -f "$out/headers/usr/include/glib-2.0/glib-object.h" ]]; then
  curl -fsSL https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/glib-dev-2.84.4-r0.apk \
    -o "$out/glib-dev.apk"
  echo '87e10319f002595ccae58356e9e2e4782b0b3f6e91bbbfa3d19f58c70a55e978  '"$out/glib-dev.apk" | sha256sum -c -
  tar --warning=no-unknown-keyword -xzf "$out/glib-dev.apk" -C "$out/headers" usr/include/glib-2.0 usr/lib/glib-2.0/include
fi
# Resolve libc/GTK from the loading application. The host musl toolchain's
# default NEEDED libc.so names PachaOS's native libc, not Alpine's Linux libc.
musl-gcc -std=c11 -O2 -g -Wall -Wextra -Werror -shared -fPIC -nostdlib \
  -I "$out/headers/usr/include/glib-2.0" -I "$out/headers/usr/lib/glib-2.0/include" \
  "$repo/tests/gtk_frame_observer.c" "$repo/tests/gtk_frame_io.c" -o "$out/frame-observer.so"
sha256sum "$repo/tests/gtk_frame_observer.c" "$out/frame-observer.so"
