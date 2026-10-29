#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$repo_root" --command bash "$0" "$@"
fi
source "$repo_root/pack/scripts/lib/jobs.sh"
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="$repo_root/.artifacts/kobox2-ps2-canonical"
output="$repo_root/.artifacts/kobox2-ps2-production"
jobs="${JOBS:-${PACGO_BUILD_JOBS:-2}}"
if [[ -n "${CAPOS_LINUX_LLVM18_BIN:-}" ]]; then
    export PATH="$CAPOS_LINUX_LLVM18_BIN:$PATH"
fi
if [[ -n "${CAPOS_LIBELF_INCLUDE:-}" && -n "${CAPOS_LIBELF_LIB:-}" ]]; then
    export HOSTCFLAGS="${HOSTCFLAGS:-} -I${CAPOS_LIBELF_INCLUDE}"
    export LIBRARY_PATH="${CAPOS_LIBELF_LIB}${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
mkdir -p "$canonical" "$output"
kconfig=$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$canonical")
profile="$source_tree/kobox/manifest/profiles/ps2.json"
if [[ "${1:-}" == --configure-only || ! -f "$canonical/.config" ||
      "$source_tree/kobox/task/config" -nt "$canonical/.config" ||
      "$source_tree/kobox/boot/config" -nt "$canonical/.config" ||
      "$source_tree/kobox/manifest/profiles/ps2.config" -nt "$canonical/.config" ]]; then
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 KBUILD_KCONFIG="$kconfig" tinyconfig
    "$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$canonical" \
        "$canonical/.config" "$source_tree/kobox/task/config" \
        "$source_tree/kobox/boot/config" "$source_tree/kobox/manifest/profiles/ps2.config"
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 KBUILD_KCONFIG="$kconfig" olddefconfig
fi
if [[ "${1:-}" == --configure-only ]]; then
    sha256sum "$canonical/.config"
    exit 0
fi
python3 - "$profile" "$canonical/.config" <<'PY'
import hashlib,json,sys
assert hashlib.sha256(open(sys.argv[2],'rb').read()).hexdigest()==json.load(open(sys.argv[1]))['config_sha256'], 'PS/2 config differs from its profile'
PY
pacgo_make "$jobs" -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 KBUILD_KCONFIG="$kconfig" vmlinux modules
python3 "$source_tree/kobox/manifest/generate_boot_core_inventory.py" \
    --source-tree "$source_tree" --build-dir "$canonical" --profile "$profile" \
    --nm llvm-nm-18 --readelf llvm-readelf-18 --output "$output/boot-core-inventory.json"
python3 "$source_tree/kobox/manifest/generate_closure_inventory.py" \
    --source-tree "$source_tree" --build-dir "$canonical" --profile "$profile" \
    --nm llvm-nm-18 --output "$output/closure-inventory.json"
identity=$(sha256sum "$canonical/.config" "$canonical/vmlinux" "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)
provider="$repo_root/.artifacts/kobox2-ps2-providers/$identity"
pacgo_reserve_jobs "$jobs" >/dev/null
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$provider" --output-dir "$output/runtime" \
    --device-profile ps2 --cc clang-18 --ld ld.lld-18 --llvm=-18 \
    --ar llvm-ar-18 --nm llvm-nm-18 --objdump llvm-objdump-18 --objcopy llvm-objcopy-18 \
    --module-inventory "$output/closure-inventory.json" --link --jobs "$PACGO_RESERVED_JOBS"
pacgo_release_jobs
printf '%s\n' "${provider#"$repo_root/"}" > "$output/provider-kbuild-dir"
python3 "$source_tree/kobox/manifest/build_pci_package.py" \
    --inventory "$output/closure-inventory.json" --runtime-dir "$output/runtime" \
    --output-dir "$output/package" --protocol-dir "$repo_root/kobox2/protocol" \
    --profile ps2 --generator-dir "$output/manifest-build"
bash "$repo_root/pack/scripts/build_kobox2_sandbox.sh" \
    "$output/sandbox.elf" 'gpud_gpu_|ph_gpu_|ph_device_|ph_dma_' --ps2
cp "$output/sandbox.elf" "$output/package/sandbox.elf"
