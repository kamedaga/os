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
lock="${repo_root}/tools/manifests/arch-linux-6.8-kobox-x86_64.lock"
read -r _pkg _version url expected_sha < <(awk '$1 == "linux" { print; exit }' "${lock}")
pkg="${url##*/}"
pkg_path="${cache}/${pkg}"
extract="${cache}/extracted"

mkdir -p "${cache}" "${extract}"

command -v zstd >/dev/null || { echo "zstd is required" >&2; exit 1; }

if [ ! -f "${pkg_path}" ] || [ "$(sha256sum "${pkg_path}" | awk '{print $1}')" != "${expected_sha}" ]; then
    curl -fL -o "${pkg_path}" "${url}"
fi
printf '%s  %s\n' "${expected_sha}" "${pkg_path}" | sha256sum -c - >/dev/null

extract_module() {
    local path="$1"
    local out_name="$2"
    local zst_path="${extract}/${path}"
    if [ ! -f "${zst_path}" ]; then
        zstd -dc "${pkg_path}" | tar -x -C "${extract}" "${path}"
    fi
    zstd -dc "${zst_path}" > "${out}/${out_name}"
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
extract_module \
    "usr/lib/modules/6.8.0-arch1-1/kernel/drivers/virtio/virtio_input.ko.zst" \
    "linux_virtio_input.ko"
if [ "${skip_console}" -eq 0 ]; then
    extract_module \
        "usr/lib/modules/6.8.0-arch1-1/kernel/drivers/char/virtio_console.ko.zst" \
        "linux_virtio_console.ko"
fi

printf '%s\n' "${out}"
