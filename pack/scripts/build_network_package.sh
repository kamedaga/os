#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="$repo_root/.artifacts/kobox2-network-production-canonical"
output="$repo_root/.artifacts/kobox2-network-production"
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

mkdir -p "$canonical" "$output"
kbuild_kconfig="$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$canonical")"
profile="$source_tree/kobox/manifest/profiles/network.json"
expected_config_digest="$(sed -n 's/.*"config_sha256": "\([0-9a-f]*\)".*/\1/p' "$profile")"
if [[ ! "$expected_config_digest" =~ ^[0-9a-f]{64}$ ]]; then
    echo 'network profile has no valid config digest' >&2
    exit 1
fi
current_config_digest=""
if [[ -f "$canonical/.config" ]]; then
    current_config_digest="$(sha256sum "$canonical/.config" | cut -d' ' -f1)"
fi
compiler_version="$(clang-18 --version | head -1)"
expected_compiler_version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["toolchain"]["compiler_version"])' "$profile")"
if [[ "$compiler_version" != *"$expected_compiler_version"* ]]; then
    echo "network profile requires clang $expected_compiler_version; run under nix develop" >&2
    exit 1
fi
recorded_compiler="$(sed -n 's/^CONFIG_CC_VERSION_TEXT="\(.*\)"$/\1/p' "$canonical/.config" 2>/dev/null || true)"
if [[ "$current_config_digest" != "$expected_config_digest" ||
      "$recorded_compiler" != "$compiler_version" ||
      "$source_tree/kobox/task/config" -nt "$canonical/.config" ||
      "$source_tree/kobox/boot/config" -nt "$canonical/.config" ||
      "$source_tree/kobox/manifest/profiles/network.config" -nt "$canonical/.config" ]]; then
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" tinyconfig
    "$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$canonical" \
        "$canonical/.config" "$source_tree/kobox/task/config" \
        "$source_tree/kobox/boot/config" \
        "$source_tree/kobox/manifest/profiles/network.config"
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" olddefconfig
fi
if [[ "$(sha256sum "$canonical/.config" | cut -d' ' -f1)" != "$expected_config_digest" ]]; then
    echo 'network canonical config differs from its pinned profile' >&2
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
provider="$repo_root/.artifacts/kobox2-network-providers/$identity"
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$provider" --output-dir "$output/runtime" \
    --device-profile network --cc clang-18 --ld ld.lld-18 \
    --llvm=-18 --ar llvm-ar-18 --nm llvm-nm-18 \
    --objdump llvm-objdump-18 --objcopy llvm-objcopy-18 \
    --module-inventory "$output/closure-inventory.json" \
    --link --jobs "$jobs"
bash "$repo_root/userland/kobox2_adapter/build-sandbox.sh" \
    "$output/sandbox.elf" --net
python3 "$source_tree/kobox/manifest/build_network_package.py" \
    --inventory "$output/closure-inventory.json" \
    --runtime-dir "$output/runtime" --output-dir "$output" \
    --protocol-dir "$repo_root/kobox2/protocol"

if llvm-nm-18 --defined-only "$output/runtime/linux-boot-runtime.so" |
    rg ' [TtDdBb] (drm_|kobox_linux_drm_|kobox_linux_input_port_)'; then
    echo 'GPU/USB input service leaked into the network hosted core' >&2
    exit 1
fi
if nm --defined-only "$output/sandbox.elf" |
    rg ' [TtDdBb] (gpud_gpu_|ph_gpu_|ph_usb_input_)'; then
    echo 'GPU/USB input service leaked into the network sandbox' >&2
    exit 1
fi
