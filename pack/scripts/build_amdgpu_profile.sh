#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build/audit only: this does not package, launch, or probe the GPU.
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
build_dir="$repo_root/.artifacts/kobox2-amdgpu-canonical"
report_dir="$repo_root/.artifacts/kobox2-amdgpu-profile"
hosted_dir="$repo_root/.artifacts/kobox2-amdgpu-hosted"
profile="$source_tree/kobox/manifest/profiles/amdgpu.json"
jobs="${JOBS:-${PACGO_BUILD_JOBS:-8}}"

if [[ -n "${CAPOS_LIBELF_INCLUDE:-}" && -n "${CAPOS_LIBELF_LIB:-}" ]]; then
    if [[ ! -f "$CAPOS_LIBELF_INCLUDE/gelf.h" ||
          ! -f "$CAPOS_LIBELF_LIB/libelf.so" ]]; then
        echo 'AMDGPU profile requires valid CAPOS_LIBELF_INCLUDE and CAPOS_LIBELF_LIB' >&2
        exit 1
    fi
    HOSTCFLAGS="${HOSTCFLAGS:-} -isystem${CAPOS_LIBELF_INCLUDE}"
    LIBRARY_PATH="${CAPOS_LIBELF_LIB}${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export HOSTCFLAGS LIBRARY_PATH
elif ! printf '#include <gelf.h>\n' | cc -E -x c - >/dev/null 2>&1; then
    echo 'AMDGPU profile requires libelf headers; set CAPOS_LIBELF_INCLUDE/LIB or install libelf-dev' >&2
    exit 1
fi

expected_compiler="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["toolchain"]["compiler_version"])' "$profile")"
if [[ "$(clang-18 --version | head -1)" != *"$expected_compiler"* ]]; then
    echo "AMDGPU profile requires clang-18 $expected_compiler" >&2
    exit 1
fi

mkdir -p "$build_dir" "$report_dir"
kbuild_kconfig="$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$build_dir")"
expected_digest="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["config_sha256"])' "$profile")"
current_digest=""
if [[ -f "$build_dir/.config" ]]; then
    current_digest="$(sha256sum "$build_dir/.config" | cut -d' ' -f1)"
fi
if [[ "$current_digest" != "$expected_digest" ||
      "$source_tree/kobox/task/config" -nt "$build_dir/.config" ||
      "$source_tree/kobox/boot/config" -nt "$build_dir/.config" ||
      "$source_tree/kobox/manifest/profiles/amdgpu.config" -nt "$build_dir/.config" ]]; then
    make -C "$source_tree" O="$build_dir" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" tinyconfig
    "$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$build_dir" \
        "$build_dir/.config" "$source_tree/kobox/task/config" \
        "$source_tree/kobox/boot/config" \
        "$source_tree/kobox/manifest/profiles/amdgpu.config"
    make -C "$source_tree" O="$build_dir" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" olddefconfig
fi
if [[ "$(sha256sum "$build_dir/.config" | cut -d' ' -f1)" != "$expected_digest" ]]; then
    echo 'AMDGPU Kconfig differs from its pinned profile' >&2
    exit 1
fi

pacgo_make "$jobs" -C "$source_tree" O="$build_dir" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kbuild_kconfig" vmlinux modules
python3 "$source_tree/kobox/manifest/generate_boot_core_inventory.py" \
    --source-tree "$source_tree" --build-dir "$build_dir" \
    --profile "$profile" --nm llvm-nm-18 --readelf llvm-readelf-18 \
    --output "$report_dir/boot-core-inventory.json"
python3 "$source_tree/kobox/manifest/generate_closure_inventory.py" \
    --source-tree "$source_tree" --build-dir "$build_dir" \
    --profile "$profile" --nm llvm-nm-18 \
    --output "$report_dir/closure-inventory.json"
modinfo -F firmware "$build_dir/drivers/gpu/drm/amd/amdgpu/amdgpu.ko" |
    LC_ALL=C sort -u > "$report_dir/declared-firmware.txt"

identity="$(sha256sum "$build_dir/.config" "$build_dir/vmlinux" \
    "$build_dir/vmlinux.a" | sha256sum | cut -d' ' -f1)"
provider_dir="$repo_root/.artifacts/kobox2-amdgpu-providers/$identity"
pacgo_reserve_jobs "$jobs" >/dev/null
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
    --source-tree "$source_tree" --canonical-build-dir "$build_dir" \
    --provider-build-dir "$provider_dir" --output-dir "$hosted_dir" \
    --device-profile amdgpu --cc clang-18 --ld ld.lld-18 \
    --llvm=-18 --ar llvm-ar-18 --nm llvm-nm-18 \
    --objdump llvm-objdump-18 --objcopy llvm-objcopy-18 \
    --module-inventory "$report_dir/closure-inventory.json" \
    --link --jobs "$PACGO_RESERVED_JOBS"
pacgo_release_jobs
printf '%s\n' "${provider_dir#"$repo_root/"}" > "$hosted_dir/provider-kbuild-dir"
echo "AMDGPU canonical and hosted link audits passed: $report_dir $hosted_dir"
echo 'Build-only audit complete. Use test_amdgpu_modules.sh for no-device load/unload; GPU probe and display are not tested here.'
