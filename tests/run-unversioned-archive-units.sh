#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/unversioned-archive-units}"
mkdir -p "$out"
cd "$repo_root"
python3 tests/test_amdgpu_firmware_bundle.py > "$out/firmware.log" 2>&1
python3 tests/test_amdgpu_mesa_package.py > "$out/mesa.log" 2>&1
python3 tests/test_live_desktop_bundle_metadata.py > "$out/desktop.log" 2>&1
bash tests/run-kobox2-owner-bootfs-unit.sh "$out/owner" > "$out/owner.log" 2>&1
# These readers dereference the native bootstrap's fixed VA. ASan's own
# mappings can occupy that address; the shared owner parser above is sanitized.
cc -std=c11 -Wall -Wextra -Werror -g \
    tests/bootfs_readers_extent_unit.c userland/seed0boot/src/bootfs_reader.c \
    -o "$out/seed-reader"
"$out/seed-reader" > "$out/seed-reader.log" 2>&1
cc -std=c11 -Wall -Wextra -Werror -g -DTEST_BOOTFS_ADAPTER \
    -Iuserland/libipc/include -Iuserland/libpacha/include \
    -Ikobox2/linux-sandbox/kobox -Ikobox2/protocol/include \
    tests/bootfs_readers_extent_unit.c userland/kobox2_adapter/bootfs.c \
    -o "$out/adapter-reader"
"$out/adapter-reader" > "$out/adapter-reader.log" 2>&1
cd "$repo_root/pack"
go test ./internal/bootfs > "$out/bootfs-go.log" 2>&1
