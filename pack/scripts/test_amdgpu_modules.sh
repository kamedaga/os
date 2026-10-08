#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Hosted firmware and load/unload only. No PCI grant or GPU probe.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
# The kobox2 core is pinned to nix's LLVM 18.1.8, and kbuild host tools
# (fixdep, objtool) must link against nix's glibc and libelf. Only this build
# enters nix; pacgo and every other build stay outside it.
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$repo_root" --command bash "$0" "$@"
fi
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="$repo_root/.artifacts/kobox2-amdgpu-canonical"
inventory="$repo_root/.artifacts/kobox2-amdgpu-profile/closure-inventory.json"
hosted="$repo_root/.artifacts/kobox2-amdgpu-gates"
cmake_dir="$repo_root/.artifacts/kobox2-amdgpu-smoke-cmake"
log="$repo_root/.artifacts/kobox2-amdgpu-module-smoke.log"
bad_hash_log="$repo_root/.artifacts/kobox2-amdgpu-firmware-reject.log"
missing_log="$repo_root/.artifacts/kobox2-amdgpu-firmware-missing.log"
firmware_dir="$repo_root/.artifacts/kobox2-amdgpu-firmware"
firmware_manifest="$source_tree/kobox/manifest/profiles/amdgpu-firmware.sha256"
probe_source="$source_tree/kobox/boot/firmware_probe"
probe_output="$repo_root/.artifacts/kobox2-amdgpu-firmware-probe"
mapping_file="$repo_root/.artifacts/kobox2-amdgpu-package/modules/drm_mapping.ko"

if [[ ! -f "$canonical/vmlinux" || ! -f "$inventory" ]]; then
    echo 'Run pack/scripts/build_amdgpu_profile.sh first' >&2
    exit 1
fi
if [[ -n "${CAPOS_LIBELF_INCLUDE:-}" && -n "${CAPOS_LIBELF_LIB:-}" ]]; then
    HOSTCFLAGS="${HOSTCFLAGS:-} -isystem${CAPOS_LIBELF_INCLUDE}"
    LIBRARY_PATH="${CAPOS_LIBELF_LIB}${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export HOSTCFLAGS LIBRARY_PATH
fi

identity="$(sha256sum "$canonical/.config" "$canonical/vmlinux" \
    "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)"
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$repo_root/.artifacts/kobox2-amdgpu-providers/$identity" \
    --output-dir "$hosted" --device-profile amdgpu --cc clang-18 \
    --ld ld.lld-18 --llvm=-18 --ar llvm-ar-18 --nm llvm-nm-18 \
    --objdump llvm-objdump-18 --objcopy llvm-objcopy-18 \
    --module-inventory "$inventory" --with-gates --link --jobs "${JOBS:-8}"

if [[ ! -d "$firmware_dir" ]]; then
    echo 'Fetch the pinned AMDGPU firmware before the no-device test' >&2
    exit 1
fi
if [[ ! -f "$mapping_file" ]]; then
    echo 'Build the AMDGPU package with its Kobox DRM mapping module first' >&2
    exit 1
fi
(cd "$firmware_dir" && sha256sum -c "$firmware_manifest")
mkdir -p "$probe_output"
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    M="$probe_source" MO="$probe_output" -j"${JOBS:-8}" modules

# The pinned libelf path is for Kbuild's host tools only. It may also contain
# a foreign libc; never let the native CMake smoke-test linker search it.
unset LIBRARY_PATH

cmake -S "$repo_root/kobox2" -B "$cmake_dir" -DBUILD_TESTING=ON \
    -DKB2_OS_BACKEND=linux -DKB2_CPU_ARCH=x86_64 -DCMAKE_BUILD_TYPE= \
    -DKOBOX_LINUX_TASK_BUILD_DIR="$canonical" \
    -DKOBOX_LINUX_BOOT_RUNTIME_CORE="$hosted/linux-boot-runtime.so"
cmake --build "$cmake_dir" --target kobox_linux_module_smoke_test -j"${JOBS:-8}"

mapfile -t module_paths < <(python3 -c \
    'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["load_order"]))' \
    "$inventory")
module_args=()
for path in "${module_paths[@]}"; do
    file="$hosted/modules/$path"
    name="$(modinfo -F name "$file")"
    if [[ "$name" == amdgpu ]]; then
        module_args+=("drm_mapping=$mapping_file")
    fi
    if [[ "$name" == firmware_class ]]; then
        module_args+=("$name=$file;dyndbg=+p")
    elif [[ "$name" == drm ]]; then
        module_args+=("$name=$file;debug=0x1")
    else
        module_args+=("$name=$file")
    fi
done
firmware_args=()
probe_requests=()
while read -r digest path; do
    [[ "$digest" == \#* || -z "$digest" ]] && continue
    firmware_args+=("--firmware=$path=$digest=$firmware_dir/$path")
    probe_requests+=("$path:$digest")
done < "$firmware_manifest"
probe_parameters=""
for ((start=0; start<${#probe_requests[@]}; start+=5)); do
    group=("${probe_requests[@]:start:5}")
    joined="$(IFS=,; printf '%s' "${group[*]}")"
    probe_parameters+="${probe_parameters:+ }requests$((start / 5))=$joined"
done
module_args+=("kobox_firmware_probe=$probe_output/kobox_firmware_probe.ko;$probe_parameters")
timeout 180 "$cmake_dir/linux-sandbox/kobox/kobox_linux_module_smoke_test" \
    "$hosted/linux-boot-runtime.so" "${firmware_args[@]}" \
    "${module_args[@]}" > "$log" 2>&1 || {
        tail -80 "$log" >&2
        exit 1
    }
expected_modules=$((${#module_paths[@]} + 2))
rg -F "Native manifest lifecycle: status=0 loaded=$expected_modules unloaded=$expected_modules cleanup=0" "$log"
for request in "${probe_requests[@]}"; do
    name="${request%%:*}"
    hash="${request#*:}"
    if ! rg -F "kobox-firmware: request=$name status=0" "$log" >/dev/null ||
       ! rg -F "Loaded FW: $name, sha256: $hash" "$log" >/dev/null; then
        echo "Firmware request missing from log: $name" >&2
        exit 1
    fi
done
first_name="${probe_requests[0]%%:*}"
bad_hash="$(printf '%064d' 0)"
if timeout 180 "$cmake_dir/linux-sandbox/kobox/kobox_linux_module_smoke_test" \
    "$hosted/linux-boot-runtime.so" \
    "--firmware=$first_name=$bad_hash=$firmware_dir/$first_name" \
    "${module_args[0]}" > "$bad_hash_log" 2>&1; then
    echo 'Corrupted firmware digest unexpectedly passed' >&2
    exit 1
fi
rg -F 'Native manifest lifecycle: status=-74 loaded=0 unloaded=0 cleanup=0' \
    "$bad_hash_log" >/dev/null
if rg -F 'kobox-firmware: staged=' "$bad_hash_log" >/dev/null; then
    echo 'Corrupted firmware was materialized before digest rejection' >&2
    exit 1
fi
missing_args=("${module_args[@]}")
missing_args[${#missing_args[@]}-1]="kobox_firmware_probe=$probe_output/kobox_firmware_probe.ko;requests0=amdgpu/not-present.bin:$bad_hash"
if timeout 180 "$cmake_dir/linux-sandbox/kobox/kobox_linux_module_smoke_test" \
    "$hosted/linux-boot-runtime.so" "${firmware_args[@]}" \
    "${missing_args[@]}" > "$missing_log" 2>&1; then
    echo 'Absent firmware request unexpectedly passed' >&2
    exit 1
fi
rg -F 'kobox-firmware: request=amdgpu/not-present.bin status=-2' \
    "$missing_log" >/dev/null
expected_before_probe=$((${#module_paths[@]} + 1))
rg -F "Native manifest lifecycle: status=-2 loaded=$expected_before_probe unloaded=$expected_before_probe cleanup=0" \
    "$missing_log" >/dev/null
if [[ "$(rg -c 'kobox-firmware: removed=' "$missing_log")" != "${#firmware_args[@]}" ]]; then
    echo 'Absent firmware path did not remove every staged file' >&2
    exit 1
fi
echo "No-device firmware request and AMDGPU closure load/unload passed; log: $log"
