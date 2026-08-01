#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image_name="pine2-gtk-builder:alpine-3.21"
binary="$project_dir/build-musl/pine2-gtk"

if [[ ! -x "$binary" ]]; then
  echo "Musl binary not found. Run ./tools/build-musl.sh first." >&2
  exit 1
fi

docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -e PINE2_GTK_SMOKE_TEST=1 \
  -v "$project_dir:/src:ro" \
  "$image_name" \
  sh -lc '
    set -eu
    ldd /src/build-musl/pine2-gtk
    xvfb-run -a /src/build-musl/pine2-gtk
  '

echo "GTK3 musl smoke test passed"
