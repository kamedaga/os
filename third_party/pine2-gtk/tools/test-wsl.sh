#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image_name="pine2-gtk-builder:ubuntu-24.04"

docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -e PINE2_GTK_SMOKE_TEST=1 \
  -v "$project_dir:/src" \
  "$image_name" \
  bash -lc '
    Xvfb :97 -screen 0 1280x1024x24 -nolisten tcp >/tmp/pine2-xvfb.log 2>&1 &
    xvfb_pid=$!
    trap "kill $xvfb_pid 2>/dev/null || true" EXIT
    DISPLAY=:97 /src/build/pine2-gtk
  '

echo "GTK3 smoke test passed"
