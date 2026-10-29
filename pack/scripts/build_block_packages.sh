#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source "$repo_root/pack/scripts/lib/jobs.sh"
# The kobox2 core is pinned to nix's LLVM 18.1.8, and kbuild host tools
# (fixdep, objtool) must link against nix's glibc and libelf. Only this build
# enters nix; pacgo and every other build stay outside it.
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$repo_root" --command bash "$0" "$@"
fi
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="$repo_root/.artifacts/kobox2-block-production-canonical"
jobs="${JOBS:-${PACGO_BUILD_JOBS:-2}}"

if [[ -n "${CAPOS_LINUX_LLVM18_BIN:-}" ]]; then
    PATH="$CAPOS_LINUX_LLVM18_BIN:$PATH"
    export PATH
fi
if [[ -n "${CAPOS_LIBELF_INCLUDE:-}" && -n "${CAPOS_LIBELF_LIB:-}" ]]; then
    HOSTCFLAGS="${HOSTCFLAGS:-} -isystem${CAPOS_LIBELF_INCLUDE}"
    LIBRARY_PATH="${CAPOS_LIBELF_LIB}${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export HOSTCFLAGS LIBRARY_PATH
fi

mkdir -p "$canonical"
kbuild_kconfig="$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$canonical")"
usb_profile="$source_tree/kobox/manifest/profiles/usb_storage_xhci.json"
nvme_profile="$source_tree/kobox/manifest/profiles/nvme.json"
config_fragment="$source_tree/kobox/manifest/profiles/block_devices.config"
usb_fragment="$source_tree/kobox/manifest/profiles/usb_hid_xhci.config"
expected_digest="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["config_sha256"])' "$usb_profile")"
nvme_digest="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["config_sha256"])' "$nvme_profile")"
[[ "$expected_digest" == "$nvme_digest" ]] || {
    echo 'USB and NVMe package profiles require different canonical cores' >&2
    exit 1
}
current_digest="$(sha256sum "$canonical/.config" 2>/dev/null | cut -d' ' -f1 || true)"
compiler_version="$(clang-18 --version | head -1)"
recorded_compiler="$(sed -n 's/^CONFIG_CC_VERSION_TEXT="\(.*\)"$/\1/p' "$canonical/.config" 2>/dev/null || true)"
if [[ "$current_digest" != "$expected_digest" ||
      "$recorded_compiler" != "$compiler_version" ||
      "$source_tree/kobox/task/config" -nt "$canonical/.config" ||
      "$source_tree/kobox/boot/config" -nt "$canonical/.config" ||
      "$usb_fragment" -nt "$canonical/.config" ||
      "$config_fragment" -nt "$canonical/.config" ]]; then
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" tinyconfig
    "$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$canonical" \
        "$canonical/.config" "$source_tree/kobox/task/config" \
        "$source_tree/kobox/boot/config" "$usb_fragment" "$config_fragment"
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" olddefconfig
fi
[[ "$(sha256sum "$canonical/.config" | cut -d' ' -f1)" == "$expected_digest" ]] || {
    echo 'block canonical config differs from pinned profiles' >&2
    exit 1
}
pacgo_make "$jobs" -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kbuild_kconfig" vmlinux modules

# The provider builder pins all three canonical inputs, not just .config.
# A rebuild with the same config can produce different objects; give that
# identity its own cache rather than reusing (or deleting) an incompatible one.
provider_identity="$(sha256sum "$canonical/.config" "$canonical/vmlinux" \
    "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)"

for profile in usb-storage-xhci nvme; do
    case "$profile" in
        usb-storage-xhci)
            manifest="$usb_profile"; runtime_profile=usb-storage ;;
        nvme)
            manifest="$nvme_profile"; runtime_profile=nvme ;;
    esac
    output="$repo_root/.artifacts/kobox2-$profile-production"
    provider="$repo_root/.artifacts/kobox2-$profile-providers/$expected_digest-$provider_identity"
    mkdir -p "$output"
    python3 "$source_tree/kobox/manifest/generate_boot_core_inventory.py" \
        --source-tree "$source_tree" --build-dir "$canonical" \
        --profile "$manifest" --nm llvm-nm-18 --readelf llvm-readelf-18 \
        --output "$output/boot-core-inventory.json"
    python3 "$source_tree/kobox/manifest/generate_closure_inventory.py" \
        --source-tree "$source_tree" --build-dir "$canonical" \
        --profile "$manifest" --nm llvm-nm-18 \
        --output "$output/closure-inventory.json"
    pacgo_reserve_jobs "$jobs" >/dev/null
    python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
        --source-tree "$source_tree" --canonical-build-dir "$canonical" \
        --provider-build-dir "$provider" --output-dir "$output/runtime" \
        --device-profile "$runtime_profile" --cc clang-18 --ld ld.lld-18 \
        --llvm=-18 --ar llvm-ar-18 --nm llvm-nm-18 \
        --objdump llvm-objdump-18 --objcopy llvm-objcopy-18 \
        --module-inventory "$output/closure-inventory.json" \
        --link --jobs "$PACGO_RESERVED_JOBS"
    pacgo_release_jobs
    printf '%s\n' "${provider#"$repo_root/"}" > "$output/provider-kbuild-dir"
    python3 "$source_tree/kobox/manifest/build_pci_package.py" \
        --inventory "$output/closure-inventory.json" \
        --runtime-dir "$output/runtime" --output-dir "$output" \
        --protocol-dir "$repo_root/kobox2/protocol" --profile "$profile"
done
