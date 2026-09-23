#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cache="${repo_root}/.artifacts/arch-linux-headers-6.8"
lock="${repo_root}/tools/manifests/arch-linux-6.8-kobox-x86_64.lock"
read -r _pkg _version url expected_sha < <(awk '$1 == "linux-headers" { print; exit }' "${lock}")
pkg="${url##*/}"
pkg_path="${cache}/${pkg}"
root="${cache}/root"
kdir="${root}/usr/lib/modules/6.8.0-arch1-1/build"

mkdir -p "${cache}"

command -v zstd >/dev/null || { echo "zstd is required" >&2; exit 1; }

if [ ! -f "${pkg_path}" ] || [ "$(sha256sum "${pkg_path}" | awk '{print $1}')" != "${expected_sha}" ]; then
    curl -fL -o "${pkg_path}" "${url}"
fi
printf '%s  %s\n' "${expected_sha}" "${pkg_path}" | sha256sum -c - >/dev/null

if [ ! -f "${kdir}/Makefile" ]; then
    rm -rf "${root}"
    mkdir -p "${root}"
    zstd -dc "${pkg_path}" | tar -xf - -C "${root}"
fi

printf '%s\n' "${kdir}"
