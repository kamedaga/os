#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="$repo_root/.artifacts/kobox2-client-exec-canonical"
jobs="${JOBS:-${PACGO_BUILD_JOBS:-2}}"

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
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kbuild_kconfig" -j"$jobs" vmlinux

identity="$(sha256sum "$canonical/.config" "$canonical/vmlinux" "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)"
provider="$repo_root/.artifacts/kobox2-client-native-module-providers/$identity"
GPUD_CANONICAL_BUILD="$canonical" GPUD_PROVIDER_BUILD="$provider" \
    exec bash "$repo_root/userland/gpud/build-package.sh"
