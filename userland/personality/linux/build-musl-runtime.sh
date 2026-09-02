#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-linux-musl-libc.so}"

case "${out}" in
  /*) out_abs="${out}" ;;
  *) out_abs="${repo_root}/${out}" ;;
esac

artifact_dir="${repo_root}/.artifacts/lpr-linux-musl-runtime"
source_dir="${artifact_dir}/source"
build_dir="${artifact_dir}/build"
build_log="${artifact_dir}/build.log"
jobs="${PACHAOS_LINUX_MUSL_JOBS:-$(nproc)}"
builtins_archive="${PACHAOS_LINUX_MUSL_LIBCC:-}"

if [[ -z "${builtins_archive}" ]]; then
  builtins_archive="$(find /usr/lib/llvm-* /usr/lib/clang -path '*libclang_rt.builtins-x86_64.a' 2>/dev/null | sort -V | tail -n 1 || true)"
fi
if [[ -z "${builtins_archive}" || ! -f "${builtins_archive}" ]]; then
  builtins_archive=""
fi

rm -rf "${artifact_dir}"
mkdir -p "${artifact_dir}" "${build_dir}" "$(dirname "${out_abs}")"
cp -a "${repo_root}/musl/upstream" "${source_dir}"
cp "${script_dir}/musl/synccall.c" "${source_dir}/src/thread/synccall.c"

make_common=(
  -f "${source_dir}/Makefile"
  "srcdir=${source_dir}"
  "ARCH=x86_64"
  "CC=clang"
  "AR=ar"
  "RANLIB=ranlib"
  "LIBCC=${builtins_archive}"
  "CFLAGS=-target x86_64-linux-musl -mno-red-zone -fno-stack-protector -O2"
  "LDFLAGS_AUTO=-Wl,-soname,libc.musl-x86_64.so.1 -Wl,--sort-section,alignment -Wl,--sort-common -Wl,--gc-sections -Wl,--hash-style=both -Wl,--exclude-libs=ALL -Wl,--dynamic-list=${source_dir}/dynamic.list"
  "LDFLAGS=-fuse-ld=lld"
)

if ! make -C "${build_dir}" -j"${jobs}" "${make_common[@]}" lib/libc.so >"${build_log}" 2>&1; then
  tail -n 100 "${build_log}" >&2
  exit 1
fi

cp "${build_dir}/lib/libc.so" "${out_abs}"

printf 'LPR Linux musl runtime built:\n'
printf '  source: musl %s\n' "$(cat "${source_dir}/VERSION")"
printf '  log: %s\n' "${build_log}"
printf '  output: %s\n' "${out_abs}"
