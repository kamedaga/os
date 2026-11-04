#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds the storage sandbox and publishes the bootfs package directory: the
# Linux side from storage_package (pci-package/) plus this sandbox.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
production="$repo_root/.artifacts/kobox2-storage-production"
output="$production"
options=()
while (($#)); do
    case "$1" in
        --lazy-arena) options+=(--lazy-arena); shift ;;
        --output-directory)
            [[ $# -ge 2 && -n "$2" ]] || { echo 'missing output directory' >&2; exit 2; }
            output="$2"; shift 2 ;;
        *) echo "unknown storage build argument: $1" >&2; exit 2 ;;
    esac
done
output="$(realpath -m -- "$output")"
# Experimental candidates must be explicit. Normal pack recipe supplies no
# options, preserving its eager policy and the existing target RAM auto value.
if ((${#options[@]})) && [[ "$output" == "$production" ]]; then
    echo 'lazy storage requires a separate --output-directory' >&2; exit 2
fi
package_source="$production/pci-package"
if ((${#options[@]})); then
    bash "$repo_root/pack/scripts/build_storage_package.sh" --lazy-arena --output-directory "$output"
    package_source="$output/pci-package"
fi
bash "$repo_root/pack/scripts/build_kobox2_sandbox.sh" "$output/sandbox.elf" \
    'gpud_gpu_|ph_gpu_|ph_usb_input_|ph_net_frame_' --storage "${options[@]}"

# Assemble beside the package, then swap, so a failed copy never leaves a
# package that mixes two builds.
rm -rf "$output/package.tmp"
cp -R "$package_source" "$output/package.tmp"
install -m 0555 "$output/sandbox.elf" "$output/package.tmp/sandbox.elf"
rm -rf "$output/package"
mv "$output/package.tmp" "$output/package"
