#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
bash "$repo/pack/scripts/build_amdgpu_mesa_gears.sh"
python3 "$repo/pack/scripts/build_amdgpu_mesa_package.py" \
    --archive-output "$repo/.artifacts/kobox2-amdgpu-mesa-runtime.tar.gz"
