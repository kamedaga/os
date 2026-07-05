#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cache="${repo_root}/.artifacts/arch-linux-headers-6.8"
tools="${repo_root}/.artifacts/tools-zstd"
pkg="linux-headers-6.8.arch1-1-x86_64.pkg.tar.zst"
url="https://archive.archlinux.org/packages/l/linux-headers/${pkg}"
pkg_path="${cache}/${pkg}"
root="${cache}/root"
kdir="${root}/usr/lib/modules/6.8.0-arch1-1/build"

mkdir -p "${cache}"

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

if [ ! -f "${kdir}/Makefile" ]; then
    rm -rf "${root}"
    mkdir -p "${root}"
    "${zstd_bin}" -dc "${pkg_path}" | tar -xf - -C "${root}"
fi

printf '%s\n' "${kdir}"
