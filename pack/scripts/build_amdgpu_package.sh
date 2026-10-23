#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Explicit AMDGPU package build. It never changes the normal live bootfs.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_tree="$repo_root/kobox2/linux-sandbox"
inventory="$repo_root/.artifacts/kobox2-amdgpu-profile/closure-inventory.json"
runtime="$repo_root/.artifacts/kobox2-amdgpu-hosted"
firmware="$repo_root/.artifacts/kobox2-amdgpu-firmware"
digest_file="$source_tree/kobox/manifest/profiles/amdgpu-firmware.sha256"
output="$repo_root/.artifacts/kobox2-amdgpu-package"
archive="$repo_root/.artifacts/kobox2-amdgpu-driver.tar.gz"
sandbox_options=(--amdgpu)
if [[ $# -eq 1 && "$1" == --cache-preflight ]]; then
    # Keep the early cache diagnostic out of the normal optional package.
    output="$repo_root/.artifacts/kobox2-amdgpu-cache-preflight-package"
    archive="$repo_root/.artifacts/kobox2-amdgpu-cache-preflight-driver.tar.gz"
    sandbox_options+=(--cache-preflight)
elif [[ $# -ne 0 ]]; then
    echo "usage: $0 [--cache-preflight]" >&2
    exit 2
fi
canonical="$repo_root/.artifacts/kobox2-amdgpu-canonical"
mapping="$repo_root/.artifacts/kobox2-amdgpu-mapping"

# Package the current core and module inventory, not whichever profile happened
# to be left by a previous manual build.
bash "$repo_root/pack/scripts/build_amdgpu_profile.sh"
bash "$repo_root/pack/scripts/fetch_amdgpu_firmware.sh"
if [[ ! -f "$runtime/linux-boot-runtime.so" || ! -f "$inventory" ||
      ! -d "$firmware" ]]; then
    echo 'Build the AMDGPU profile and fetch pinned firmware first' >&2
    exit 1
fi
(cd "$firmware" && sha256sum --check "$digest_file")
identity="$(sha256sum "$canonical/.config" "$canonical/vmlinux" \
    "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)"
python3 "$source_tree/kobox/boot/build_gem_modules.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$repo_root/.artifacts/kobox2-amdgpu-providers/$identity" \
    --output-dir "$mapping" --core-dir "$runtime" \
    --mapping-only --jobs "${JOBS:-8}"
python3 "$source_tree/kobox/manifest/build_pci_package.py" \
    --profile amdgpu --inventory "$inventory" --runtime-dir "$runtime" \
    --output-dir "$output" --protocol-dir "$repo_root/kobox2/protocol" \
    --extension-module "$mapping/kobox/gem/drm_mapping.ko"
install -d "$output/firmware/amdgpu"
while read -r digest name; do
    [[ "$digest" == \#* || -z "$digest" ]] && continue
    install -m 0644 "$firmware/$name" "$output/firmware/$name"
done < "$digest_file"
install -m 0644 "$digest_file" "$output/firmware.sha256"
(cd "$output/firmware" && sha256sum --check "$output/firmware.sha256")
python3 "$repo_root/pack/scripts/build_amdgpu_firmware_bundle.py" \
    --manifest "$digest_file" --firmware-dir "$firmware" \
    --output "$output/firmware.bundle"
owner_digest="$(sed -n 's/^firmware_sha256=//p' \
    "$repo_root/pack/live-amdgpu-probe.conf")"
actual_digest="$(sha256sum "$output/firmware.bundle" | cut -d' ' -f1)"
if [[ "$owner_digest" != "$actual_digest" ]]; then
    echo 'AMDGPU bundle does not match the pinned gpud owner digest' >&2
    exit 1
fi
bash "$repo_root/userland/kobox2_adapter/build-sandbox.sh" \
    "$output/sandbox.elf" "${sandbox_options[@]}"
if nm --defined-only "$output/sandbox.elf" |
    rg ' [TtDdBb] (ph_usb_input_|ph_net_frame_)'; then
    echo 'Unrelated device service leaked into the AMDGPU sandbox' >&2
    exit 1
fi
manifest_digest="$(sha256sum "$output/manifest.bin" | cut -d' ' -f1)"
sandbox_digest="$(sha256sum "$output/sandbox.elf" | cut -d' ' -f1)"
install -m 0644 "$repo_root/pack/live-amdgpu-probe.conf" \
    "$output/owner-policy.conf"
printf 'manifest_sha256=%s\nsandbox_sha256=%s\n' \
    "$manifest_digest" "$sandbox_digest" >> "$output/owner-policy.conf"
bash "$repo_root/pack/scripts/build_amdgpu_driver_archive.sh" "$output" "$archive"
echo "AMDGPU optional package staged: $output"
echo 'No GPU probe was performed; the separate live overlay only arms manual control.'
