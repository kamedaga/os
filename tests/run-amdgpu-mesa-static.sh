#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# No PCI, network, USB writes, GPU probe or renderer fallback in this gate.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/tests/amdgpu-mesa-static
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror tests/amdgpu_uapi_layout_unit.c \
  -o .artifacts/tests/amdgpu-mesa-static/uapi-layout
.artifacts/tests/amdgpu-mesa-static/uapi-layout
for test in lpr-amdgpu-info lpr-drm-connection gpud-drm-translate \
  gpud-gpu-query gpud-drm-connection gpud-drm-capacity gpud-drm-fence \
  gpud-drm-files lpr-drm-mapping-lease; do
  bash "tests/run-$test-unit.sh"
done
echo 'AMDGPU Mesa static compatibility gate PASS (not a hardware render test)'
