#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out_arg="${1:-${repo_root}/.artifacts/kobox-drm}"
cache="${repo_root}/.artifacts/arch-linux-6.8-scan"
tools="${repo_root}/.artifacts/tools-zstd"
pkg="linux-6.8.arch1-1-x86_64.pkg.tar.zst"
url="https://archive.archlinux.org/packages/l/linux/${pkg}"
pkg_path="${cache}/${pkg}"
extract="${cache}/drm-extracted"

mkdir -p "${out_arg}" "${cache}" "${extract}"
out=$(CDPATH= cd -- "${out_arg}" && pwd)

if [ ! -x "${tools}/root/usr/bin/zstd" ]; then
    mkdir -p "${tools}/deb" "${tools}/root"
    (
        cd "${tools}/deb"
        apt-get download zstd >/tmp/apt-download-zstd.log 2>&1
        dpkg-deb -x zstd_*.deb ../root
    )
fi
zstd_bin="${tools}/root/usr/bin/zstd"

if [ ! -f "${pkg_path}" ]; then
    curl -fL -o "${pkg_path}" "${url}"
fi

extract_module() {
    local path="$1"
    local out_name="$2"
    local compressed="${extract}/${path}"
    if [ ! -f "${compressed}" ]; then
        "${zstd_bin}" -dc "${pkg_path}" | tar -x -C "${extract}" "${path}"
    fi
    "${zstd_bin}" -dc "${compressed}" > "${out}/${out_name}"
}

extract_module \
    "usr/lib/modules/6.8.0-arch1-1/kernel/drivers/virtio/virtio_dma_buf.ko.zst" \
    "linux_virtio_dma_buf.ko"
extract_module \
    "usr/lib/modules/6.8.0-arch1-1/kernel/drivers/gpu/drm/virtio/virtio-gpu.ko.zst" \
    "linux_virtio_gpu.ko"

printf '%s\n' "${out}"
