#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Hosted firmware and load/unload only. No PCI grant or GPU probe.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source "$repo_root/pack/scripts/lib/jobs.sh"
jobs="${JOBS:-${PACGO_BUILD_JOBS:-8}}"
# The kobox2 core is pinned to nix's LLVM 18.1.8, and kbuild host tools
# (fixdep, objtool) must link against nix's glibc and libelf. Only this build
# enters nix; pacgo and every other build stay outside it.
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$repo_root" --command bash "$0" "$@"
fi
source_tree="$repo_root/kobox2/linux-sandbox"
export PATH="$CAPOS_LINUX_LLVM18_BIN:$PATH"
output="$repo_root/.artifacts/intel-gpu-bringup"
canonical="$output/i915-canonical"
inventory="$output/closure-inventory.json"
hosted="$output/runtime"
cmake_dir="$output/no-device-cmake"
log="$repo_root/.artifacts/kobox2-i915-module-smoke.log"
bad_hash_log="$repo_root/.artifacts/kobox2-i915-firmware-reject.log"
missing_log="$repo_root/.artifacts/kobox2-i915-firmware-missing.log"
firmware_dir="$output/firmware"
firmware_manifest="$source_tree/kobox/manifest/profiles/i915-firmware.sha256"
probe_source="$source_tree/kobox/boot/firmware_probe"
probe_output="$output/firmware-probe"

bash "$repo_root/pack/scripts/build_i915_profile.sh" --with-gates
bash "$repo_root/pack/scripts/fetch_i915_firmware.sh"
if [[ -n "${CAPOS_LIBELF_INCLUDE:-}" && -n "${CAPOS_LIBELF_LIB:-}" ]]; then
    HOSTCFLAGS="${HOSTCFLAGS:-} -isystem${CAPOS_LIBELF_INCLUDE}"
    LIBRARY_PATH="${CAPOS_LIBELF_LIB}${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export HOSTCFLAGS LIBRARY_PATH
fi

if [[ ! -d "$firmware_dir" ]]; then
    echo 'Fetch the pinned i915 firmware before the no-device test' >&2
    exit 1
fi
(cd "$firmware_dir" && sha256sum -c "$firmware_manifest")
mkdir -p "$probe_output"
kbuild_kconfig="$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" --source-tree "$source_tree" --build-dir "$canonical")"
pacgo_make "$jobs" -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kbuild_kconfig" M="$probe_source" MO="$probe_output" modules
cache_probe_output="$output/cache-flush-probe"
mkdir -p "$cache_probe_output"
pacgo_make "$jobs" -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kbuild_kconfig" M="$source_tree/kobox/boot/cache_flush_probe" \
    MO="$cache_probe_output" modules

# The pinned libelf path is for Kbuild's host tools only. It may also contain
# a foreign libc; never let the native CMake smoke-test linker search it.
unset LIBRARY_PATH

cmake -S "$repo_root/kobox2" -B "$cmake_dir" -DBUILD_TESTING=ON \
    -DKB2_OS_BACKEND=linux -DKB2_CPU_ARCH=x86_64 -DCMAKE_BUILD_TYPE= \
    -DKOBOX_LINUX_TASK_BUILD_DIR="$canonical" \
    -DKOBOX_LINUX_BOOT_RUNTIME_CORE="$hosted/linux-boot-runtime.so"
pacgo_cmake_build "$jobs" "$cmake_dir" --target kobox_linux_module_smoke_test

mapfile -t module_paths < <(python3 -c \
    'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["load_order"]))' \
    "$inventory")
module_args=()
for path in "${module_paths[@]}"; do
    file="$hosted/modules/$path"
    name="$(modinfo -F name "$file")"
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
expected_modules=$((${#module_paths[@]} + 1))
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
cache_log="$output/i915-cache-flush-smoke.log"
timeout 180 "$cmake_dir/linux-sandbox/kobox/kobox_linux_module_smoke_test" \
    "$hosted/linux-boot-runtime.so" "${firmware_args[@]}" "${module_args[@]}" \
    "kobox_cache_flush_probe=$cache_probe_output/kobox_cache_flush_probe.ko" \
    > "$cache_log" 2>&1 || { tail -80 "$cache_log" >&2; exit 1; }
rg -F 'kobox-cache: DRM pages, sg and unaligned virtual flush PASS' "$cache_log"
cache_modules=$((expected_modules + 1))
rg -F "Native manifest lifecycle: status=0 loaded=$cache_modules unloaded=$cache_modules cleanup=0" "$cache_log"
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
missing_args[${#missing_args[@]}-1]="kobox_firmware_probe=$probe_output/kobox_firmware_probe.ko;requests0=i915/not-present.bin:$bad_hash"
if timeout 180 "$cmake_dir/linux-sandbox/kobox/kobox_linux_module_smoke_test" \
    "$hosted/linux-boot-runtime.so" "${firmware_args[@]}" \
    "${missing_args[@]}" > "$missing_log" 2>&1; then
    echo 'Absent firmware request unexpectedly passed' >&2
    exit 1
fi
rg -F 'kobox-firmware: request=i915/not-present.bin status=-2' \
    "$missing_log" >/dev/null
expected_before_probe=${#module_paths[@]}
rg -F "Native manifest lifecycle: status=-2 loaded=$expected_before_probe unloaded=$expected_before_probe cleanup=0" \
    "$missing_log" >/dev/null
if [[ "$(rg -c 'kobox-firmware: removed=' "$missing_log")" != "${#firmware_args[@]}" ]]; then
    echo 'Absent firmware path did not remove every staged file' >&2
    exit 1
fi
echo "No-device firmware request and i915 closure load/unload passed; log: $log"
