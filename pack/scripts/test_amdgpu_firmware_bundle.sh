#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output="$repo_root/.artifacts/kobox2-amdgpu-regression"
bundle="$repo_root/.artifacts/kobox2-amdgpu-package/firmware.bundle"
if [[ ! -f "$bundle" ]]; then
    echo 'Build the optional AMDGPU package first' >&2
    exit 1
fi
mkdir -p "$output"
cc -std=c11 -Wall -Wextra -Werror \
    -I"$repo_root/userland/kobox2_adapter" \
    -I"$repo_root/kobox2/linux-sandbox/kobox" \
    -I"$repo_root/kobox2/protocol/include" \
    "$repo_root/tests/amdgpu_firmware_bundle_test.c" \
    "$repo_root/userland/kobox2_adapter/firmware_bundle.c" \
    "$repo_root/kobox2/protocol/src/sha256.c" \
    -o "$output/firmware_bundle_test"
"$output/firmware_bundle_test" "$bundle"
