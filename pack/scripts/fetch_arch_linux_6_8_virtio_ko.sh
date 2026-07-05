#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out_arg="${1:-${repo_root}/.artifacts/kobox-linux-tty}"
skip_console=0
if [ "${2:-}" = "--skip-console" ]; then
    skip_console=1
fi
mkdir -p "${out_arg}"
out=$(CDPATH= cd -- "${out_arg}" && pwd)
cache="${repo_root}/.artifacts/arch-linux-6.8-scan"
tools="${repo_root}/.artifacts/tools-zstd"
pkg="linux-6.8.arch1-1-x86_64.pkg.tar.zst"
url="https://archive.archlinux.org/packages/l/linux/${pkg}"
pkg_path="${cache}/${pkg}"
extract="${cache}/extracted"

mkdir -p "${cache}" "${extract}"

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
    local zst_path="${extract}/${path}"
    if [ ! -f "${zst_path}" ]; then
        "${zstd_bin}" -dc "${pkg_path}" | tar -x -C "${extract}" "${path}"
    fi
    "${zstd_bin}" -dc "${zst_path}" > "${out}/${out_name}"
}

extract_module \
    "usr/lib/modules/6.8.0-arch1-1/kernel/drivers/virtio/virtio_pci_modern_dev.ko.zst" \
    "linux_virtio_pci_modern_dev.ko"
extract_module \
    "usr/lib/modules/6.8.0-arch1-1/kernel/drivers/virtio/virtio_pci_legacy_dev.ko.zst" \
    "linux_virtio_pci_legacy_dev.ko"
extract_module \
    "usr/lib/modules/6.8.0-arch1-1/kernel/drivers/virtio/virtio_pci.ko.zst" \
    "linux_virtio_pci.ko"
if [ "${skip_console}" -eq 0 ]; then
    extract_module \
        "usr/lib/modules/6.8.0-arch1-1/kernel/drivers/char/virtio_console.ko.zst" \
        "linux_virtio_console.ko"
fi

printf '%s\n' "${out}"
