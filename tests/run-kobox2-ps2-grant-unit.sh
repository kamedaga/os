#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output="$repo_root/.artifacts/tests/kobox2-ps2-grant-unit"
mkdir -p "$output"
clang -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
    -I "$repo_root/userland/libcapsule/include" -I "$repo_root/kobox2/linux-sandbox/kobox" \
    -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
    "$repo_root/tests/kobox2_ps2_grant_unit.c" \
    "$repo_root/userland/kobox2_adapter/ps2_native.c" \
    "$repo_root/kobox2/protocol/src/closure_manifest.c" \
    "$repo_root/kobox2/protocol/src/resource_grant.c" \
    "$repo_root/kobox2/protocol/src/sha256.c" \
    -Wl,--gc-sections -o "$output/test"
"$output/test"
