#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../.." && pwd)"
source "${repo_root}/pack/scripts/lib/jobs.sh"

artifact_dir="$(realpath -m "${PACHAOS_MUSL_RUNTIME_DIR:-${repo_root}/.artifacts/musl-pachaos-runtime}")"
# ABI comparisons need independent runtimes without overwriting a verified
# baseline. Constrain the clean rebuild to a concrete artifact subdirectory.
case "$artifact_dir" in
  "$repo_root/.artifacts/"?*) ;;
  *) echo 'native runtime output must be an artifact subdirectory' >&2; exit 2 ;;
esac
build_dir="${artifact_dir}/build"
install_dir="${artifact_dir}/install"
# Never deploy the native syscall ABI over Alpine's Linux /lib interpreter.
rootfs_lib_dir="${artifact_dir}/rootfs/usr/lib/pachaos"
build_log="${artifact_dir}/build.log"
jobs="${PACHAOS_MUSL_JOBS:-${PACGO_BUILD_JOBS:-$(nproc)}}"
# The compiler's own compiler-rt builtins, so libc.so links the runtime
# helpers of the clang that compiled it.
builtins_archive="${PACHAOS_MUSL_LIBCC:-$(clang --rtlib=compiler-rt -print-libgcc-file-name)}"
if [ ! -f "${builtins_archive}" ]; then
  echo "build-runtime.sh: compiler-rt builtins not found: ${builtins_archive}" >&2
  exit 1
fi

rm -rf "${artifact_dir}"
mkdir -p "${build_dir}" "${install_dir}/usr/lib" "${rootfs_lib_dir}"

make_common=(
  -f "${repo_root}/musl/pachaos/build/runtime.mk"
  "srcdir=${repo_root}/musl/upstream"
  "ARCH=pachaos"
  "syslibdir=/usr/lib/pachaos"
  "CC=clang"
  "AR=ar"
  "RANLIB=ranlib"
  "LIBCC=${builtins_archive}"
  "CFLAGS=-target x86_64-linux-musl -mno-red-zone -fno-stack-protector -I${repo_root}/musl/pachaos/include -D__pachaos__ -O2"
  "LDFLAGS_AUTO=-Wl,--sort-section,alignment -Wl,--sort-common -Wl,--gc-sections -Wl,--hash-style=both -Wl,--exclude-libs=ALL -Wl,--dynamic-list=${repo_root}/musl/upstream/dynamic.list"
  "LDFLAGS=-fuse-ld=lld"
)

if ! pacgo_make "${jobs}" -C "${build_dir}" "${make_common[@]}" \
  lib/libc.so lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o lib/Scrt1.o lib/rcrt1.o >"${build_log}" 2>&1; then
  tail -n 80 "${build_log}" >&2
  exit 1
fi

cp "${build_dir}/lib/crt1.o" "${install_dir}/usr/lib/crt1.o"
cp "${build_dir}/lib/crti.o" "${install_dir}/usr/lib/crti.o"
cp "${build_dir}/lib/crtn.o" "${install_dir}/usr/lib/crtn.o"
cp "${build_dir}/lib/Scrt1.o" "${install_dir}/usr/lib/Scrt1.o"
cp "${build_dir}/lib/rcrt1.o" "${install_dir}/usr/lib/rcrt1.o"
cp "${build_dir}/lib/libc.a" "${install_dir}/usr/lib/libc.a"
cp "${build_dir}/lib/libc.so" "${install_dir}/usr/lib/libc.so"

cp "${build_dir}/lib/libc.so" "${rootfs_lib_dir}/libc.so"
cp "${build_dir}/lib/libc.so" "${rootfs_lib_dir}/ld-musl-x86_64.so.1"

printf 'PachaOS musl runtime built:\n'
printf '  log: %s\n' "${build_log}"
printf '  %s\n' "${install_dir}/usr/lib/crt1.o"
printf '  %s\n' "${install_dir}/usr/lib/crti.o"
printf '  %s\n' "${install_dir}/usr/lib/crtn.o"
printf '  %s\n' "${install_dir}/usr/lib/Scrt1.o"
printf '  %s\n' "${install_dir}/usr/lib/rcrt1.o"
printf '  %s\n' "${install_dir}/usr/lib/libc.a"
printf '  %s\n' "${install_dir}/usr/lib/libc.so"
printf '  %s\n' "${rootfs_lib_dir}/ld-musl-x86_64.so.1"
