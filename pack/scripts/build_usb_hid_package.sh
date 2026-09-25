#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="$repo_root/.artifacts/kobox2-usb-hid-production-canonical"
output="$repo_root/.artifacts/kobox2-usb-hid-production"
jobs="${JOBS:-${PACGO_BUILD_JOBS:-2}}"

if [[ -n "${CAPOS_LINUX_LLVM18_BIN:-}" ]]; then
    PATH="$CAPOS_LINUX_LLVM18_BIN:$PATH"
    export PATH
fi
if [[ -n "${CAPOS_LIBELF_INCLUDE:-}" && -n "${CAPOS_LIBELF_LIB:-}" ]]; then
    HOSTCFLAGS="${HOSTCFLAGS:-} -I${CAPOS_LIBELF_INCLUDE}"
    LIBRARY_PATH="${CAPOS_LIBELF_LIB}${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export HOSTCFLAGS LIBRARY_PATH
fi

mkdir -p "$canonical" "$output"
kbuild_kconfig="$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$canonical")"
profile="$source_tree/kobox/manifest/profiles/usb_hid_xhci.json"
expected_config_digest="$(sed -n 's/.*"config_sha256": "\([0-9a-f]*\)".*/\1/p' "$profile")"
if [[ ! "$expected_config_digest" =~ ^[0-9a-f]{64}$ ]]; then
    echo 'USB HID profile has no valid config digest' >&2
    exit 1
fi
current_config_digest=""
if [[ -f "$canonical/.config" ]]; then
    current_config_digest="$(sha256sum "$canonical/.config" | cut -d' ' -f1)"
fi
if [[ "$current_config_digest" != "$expected_config_digest" ||
      "$source_tree/kobox/task/config" -nt "$canonical/.config" ||
      "$source_tree/kobox/boot/config" -nt "$canonical/.config" ||
      "$source_tree/kobox/manifest/profiles/usb_hid_xhci.config" -nt "$canonical/.config" ]]; then
    # A prior merge can leave a different, still-valid .config. Recreate the
    # pinned profile from its seed instead of compiling the wrong inventory.
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" tinyconfig
    "$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$canonical" \
        "$canonical/.config" "$source_tree/kobox/task/config" \
        "$source_tree/kobox/boot/config" \
        "$source_tree/kobox/manifest/profiles/usb_hid_xhci.config"
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" olddefconfig
fi
if [[ "$(sha256sum "$canonical/.config" | cut -d' ' -f1)" != "$expected_config_digest" ]]; then
    echo 'USB HID canonical config differs from its pinned profile' >&2
    exit 1
fi
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kbuild_kconfig" -j"$jobs" vmlinux modules

python3 "$source_tree/kobox/manifest/generate_boot_core_inventory.py" \
    --source-tree "$source_tree" --build-dir "$canonical" \
    --profile "$profile" --nm llvm-nm-18 --readelf llvm-readelf-18 \
    --output "$output/boot-core-inventory.json"
python3 "$source_tree/kobox/manifest/generate_closure_inventory.py" \
    --source-tree "$source_tree" --build-dir "$canonical" \
    --profile "$profile" --nm llvm-nm-18 \
    --output "$output/closure-inventory.json"

identity="$(sha256sum "$canonical/.config" "$canonical/vmlinux" \
    "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)"
provider="$repo_root/.artifacts/kobox2-usb-hid-providers/$identity"
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$provider" --output-dir "$output/runtime" \
    --device-profile usb-hid --cc clang-18 --ld ld.lld-18 \
    --llvm=-18 --ar llvm-ar-18 --nm llvm-nm-18 \
    --objdump llvm-objdump-18 --objcopy llvm-objcopy-18 \
    --link --jobs "$jobs"
bash "$repo_root/userland/kobox2_adapter/build-sandbox.sh" \
    "$output/sandbox.elf" --usb-hid

# The USB closure must not silently pick up GPU services when shared boot
# sources change; the two native sandboxes own different device protocols.
if llvm-nm-18 --defined-only "$output/runtime/linux-boot-runtime.so" |
    rg ' [TtDdBb] (drm_|kobox_linux_drm_|kobox_linux_device_(prepare|ready|finish|module_ready|quiesce|service))'; then
    echo 'GPU/DRM code leaked into the USB hosted core' >&2
    exit 1
fi
if nm --defined-only "$output/sandbox.elf" |
    rg ' [TtDdBb] (gpud_|ph_gpu_|kb2_gpu_)'; then
    echo 'GPU service code leaked into the USB sandbox' >&2
    exit 1
fi
