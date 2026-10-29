#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Adds the AMDGPU sandbox to the package that build_amdgpu_package.sh staged,
# pins both digests in the owner policy, and archives the driver package.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output="$repo_root/.artifacts/kobox2-amdgpu-package"
archive="$repo_root/.artifacts/kobox2-amdgpu-driver.tar.gz"
sandbox_options=(--amdgpu)
if [[ $# -eq 1 && "$1" == --cache-preflight ]]; then
    output="$repo_root/.artifacts/kobox2-amdgpu-cache-preflight-package"
    archive="$repo_root/.artifacts/kobox2-amdgpu-cache-preflight-driver.tar.gz"
    sandbox_options+=(--cache-preflight)
elif [[ $# -ne 0 ]]; then
    echo "usage: $0 [--cache-preflight]" >&2
    exit 2
fi
if [[ ! -f "$output/manifest.bin" ]]; then
    echo "Stage the AMDGPU Linux package first: build_amdgpu_package.sh $*" >&2
    exit 1
fi

bash "$repo_root/pack/scripts/build_kobox2_sandbox.sh" "$output/sandbox.elf" \
    'ph_usb_input_|ph_net_frame_' "${sandbox_options[@]}"
manifest_digest="$(sha256sum "$output/manifest.bin" | cut -d' ' -f1)"
sandbox_digest="$(sha256sum "$output/sandbox.elf" | cut -d' ' -f1)"
install -m 0644 "$repo_root/pack/live-amdgpu-probe.conf" \
    "$output/owner-policy.conf"
printf 'manifest_sha256=%s\nsandbox_sha256=%s\n' \
    "$manifest_digest" "$sandbox_digest" >> "$output/owner-policy.conf"
bash "$repo_root/pack/scripts/build_amdgpu_driver_archive.sh" "$output" "$archive"
echo "AMDGPU optional package staged: $output"
echo 'No GPU probe was performed; the separate live overlay only arms manual control.'
