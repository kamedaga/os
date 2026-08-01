#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image_name="pine2-gtk-builder:alpine-3.21"
build_dir="build-musl"

docker build \
  -t "$image_name" \
  -f "$project_dir/tools/Dockerfile.musl" \
  "$project_dir"

docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "$project_dir:/src" \
  "$image_name" \
  sh -lc "
    set -eu
    meson setup '$build_dir' --wipe --buildtype=release
    meson compile -C '$build_dir'
    strip --strip-unneeded '$build_dir/pine2-gtk'
    ldd '$build_dir/pine2-gtk'
    interpreter=\"\$(readelf -l '$build_dir/pine2-gtk' | sed -n 's/.*Requesting program interpreter: \\(.*\\)]/\\1/p')\"
    case \"\$interpreter\" in
      *ld-musl-*) ;;
      *) echo \"Expected a musl interpreter, got: \${interpreter:-unknown}\" >&2; exit 1 ;;
    esac
    echo \"Interpreter: \$interpreter\"
  "

echo "Built musl binary: $project_dir/$build_dir/pine2-gtk"
