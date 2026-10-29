#!/usr/bin/env bash
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
jobs="${JOBS:-${PACGO_BUILD_JOBS:-2}}"
canonical="$repo_root/.artifacts/kobox2-client-exec-canonical"

if [[ -n "${CAPOS_LINUX_LLVM18_BIN:-}" ]]; then
    PATH="$CAPOS_LINUX_LLVM18_BIN:$PATH"
    export PATH
fi

mkdir -p "$canonical"
kbuild_kconfig="$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$canonical")"
if [[ ! -f "$canonical/.config" ]]; then
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
        KBUILD_KCONFIG="$kbuild_kconfig" tinyconfig
fi
KBUILD_KCONFIG="$kbuild_kconfig" \
    "$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$canonical" \
    "$canonical/.config" \
    "$source_tree/kobox/manifest/profiles/virtio_gpu_virgl.config" \
    "$source_tree/kobox/task/config" \
    "$source_tree/kobox/boot/config"
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kbuild_kconfig" olddefconfig
pacgo_make "$jobs" -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kbuild_kconfig" vmlinux

identity="$(sha256sum "$canonical/.config" "$canonical/vmlinux" "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)"
provider="$repo_root/.artifacts/kobox2-client-native-module-providers/$identity"
pacgo_reserve_jobs "$jobs" >/dev/null
GPUD_CANONICAL_BUILD="$canonical" GPUD_PROVIDER_BUILD="$provider" \
    JOBS="$PACGO_RESERVED_JOBS" bash "$repo_root/userland/gpud/build-package.sh"
pacgo_release_jobs
printf '%s\n' "${provider#"$repo_root/"}" > \
    "$repo_root/.artifacts/gpud-production-runtime/provider-kbuild-dir"
