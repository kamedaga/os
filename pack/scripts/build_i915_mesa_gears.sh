#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Reuse the physical KMS/EGL gears client with strict Intel hardware checks.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
export MESA_GEARS_DRM_DRIVER=i915
exec bash "$repo/pack/scripts/build_amdgpu_mesa_gears.sh" \
    "${1:-$repo/.artifacts/i915-mesa-gears-linux.elf}"
