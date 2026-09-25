#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
exec python3 "$repo_root/pack/scripts/alpine_packages.py" materialize \
    --lock "$repo_root/tools/manifests/alpine-wayland-dev-v3.22-x86_64.lock" \
    --cache "$repo_root/.artifacts/third_party/alpine-wayland-dev-v3.22-x86_64" \
    --output "$repo_root/.artifacts/userland-fixtures/alpine-wayland-dev-root"
