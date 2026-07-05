#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
prefix="${1:-${repo_root}/.artifacts/tools}"
downloads="${repo_root}/.artifacts/downloads"
build_root="${repo_root}/.artifacts/tool-build"

bison_version=3.8.2
flex_version=2.6.4
elfutils_version=0.191

mkdir -p "${prefix}" "${downloads}" "${build_root}"

download() {
    local url="$1"
    local out="$2"
    if [ ! -f "${out}" ]; then
        curl -L --fail --retry 3 "${url}" -o "${out}"
    fi
}

if [ ! -x "${prefix}/bin/bison" ]; then
    bison_archive="${downloads}/bison-${bison_version}.tar.xz"
    download "https://ftp.gnu.org/gnu/bison/bison-${bison_version}.tar.xz" "${bison_archive}"
    rm -rf "${build_root}/bison-${bison_version}"
    tar -xJf "${bison_archive}" -C "${build_root}"
    (
        cd "${build_root}/bison-${bison_version}"
        ./configure --prefix="${prefix}" --disable-nls
        make -j"${JOBS:-2}"
        make install
    )
fi

if [ ! -x "${prefix}/bin/flex" ]; then
    flex_archive="${downloads}/flex-${flex_version}.tar.gz"
    download "https://github.com/westes/flex/releases/download/v${flex_version}/flex-${flex_version}.tar.gz" "${flex_archive}"
    rm -rf "${build_root}/flex-${flex_version}"
    tar -xzf "${flex_archive}" -C "${build_root}"
    (
        cd "${build_root}/flex-${flex_version}"
        PATH="${prefix}/bin:${PATH}" ./configure --prefix="${prefix}" --disable-shared
        PATH="${prefix}/bin:${PATH}" make -j"${JOBS:-2}"
        PATH="${prefix}/bin:${PATH}" make install
    )
fi

if [ ! -f "${prefix}/include/libelf.h" ] || [ ! -f "${prefix}/lib/libelf.a" ]; then
    elfutils_archive="${downloads}/elfutils-${elfutils_version}.tar.bz2"
    download "https://sourceware.org/elfutils/ftp/${elfutils_version}/elfutils-${elfutils_version}.tar.bz2" "${elfutils_archive}"
    rm -rf "${build_root}/elfutils-${elfutils_version}"
    tar -xjf "${elfutils_archive}" -C "${build_root}"
    (
        cd "${build_root}/elfutils-${elfutils_version}"
        ./configure \
            --prefix="${prefix}" \
            --disable-debuginfod \
            --disable-libdebuginfod \
            --disable-nls \
            --without-bzlib \
            --without-lzma \
            --without-zstd
        make -j"${JOBS:-2}" -C lib
        make -j"${JOBS:-2}" -C libelf
        make -C lib install
        make -C libelf install
    )
fi

printf '%s\n' "${prefix}/bin"
