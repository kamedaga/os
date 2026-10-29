#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the isolated Linux driver closure. Native owner/auth integration is
# built separately, so a driver-only package cannot become a boot default.
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$repo_root" --command bash "$0" "$@"
fi
source "$repo_root/pack/scripts/lib/jobs.sh"
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="$repo_root/.artifacts/kobox2-wifi-canonical"
inventory="$repo_root/.artifacts/kobox2-wifi-profile/closure-inventory.json"
output="$repo_root/.artifacts/kobox2-wifi-production"
jobs="${JOBS:-${PACGO_BUILD_JOBS:-4}}"
bash "$repo_root/pack/scripts/build_wifi_profile.sh"
export PATH="${CAPOS_LINUX_LLVM18_BIN:?}:$PATH"
export HOSTCFLAGS="${HOSTCFLAGS:-} -isystem${CAPOS_LIBELF_INCLUDE:?}"
export LIBRARY_PATH="${CAPOS_LIBELF_LIB:?}${LIBRARY_PATH:+:$LIBRARY_PATH}"
identity=$(sha256sum "$canonical/.config" "$canonical/vmlinux" "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)
provider="$repo_root/.artifacts/kobox2-wifi-providers/$identity"
pacgo_reserve_jobs "$jobs" >/dev/null
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$provider" --output-dir "$output/runtime" \
    --device-profile wifi --cc clang-18 --ld ld.lld-18 --llvm=-18 \
    --ar llvm-ar-18 --nm llvm-nm-18 --objdump llvm-objdump-18 --objcopy llvm-objcopy-18 \
    --module-inventory "$inventory" --link --jobs "$PACGO_RESERVED_JOBS"
pacgo_release_jobs
python3 "$source_tree/kobox/manifest/build_pci_package.py" \
    --inventory "$inventory" --runtime-dir "$output/runtime" \
    --output-dir "$output/linux-package" --protocol-dir "$repo_root/kobox2/protocol" \
    --profile wifi --generator-dir "$output/manifest-build"
python3 "$repo_root/pack/scripts/fetch_wifi_firmware.py"
echo "Wi-Fi Linux closure built; boot policy unchanged: $output/linux-package"
