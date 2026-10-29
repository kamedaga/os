#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds the storage sandbox and publishes the bootfs package directory: the
# Linux side from storage_package (pci-package/) plus this sandbox.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output="$repo_root/.artifacts/kobox2-storage-production"
bash "$repo_root/pack/scripts/build_kobox2_sandbox.sh" "$output/sandbox.elf" \
    'gpud_gpu_|ph_gpu_|ph_usb_input_|ph_net_frame_' --storage

# Assemble beside the package, then swap, so a failed copy never leaves a
# package that mixes two builds.
rm -rf "$output/package.tmp"
cp -R "$output/pci-package" "$output/package.tmp"
install -m 0555 "$output/sandbox.elf" "$output/package.tmp/sandbox.elf"
rm -rf "$output/package"
mv "$output/package.tmp" "$output/package"
