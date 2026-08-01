#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image_name="pine2-gtk-builder:ubuntu-24.04"

docker build -t "$image_name" -f "$project_dir/tools/Dockerfile.build" "$project_dir"
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "$project_dir:/src" \
  "$image_name" \
  bash -lc 'meson setup build --wipe && meson compile -C build'

echo "Built: $project_dir/build/pine2-gtk"
