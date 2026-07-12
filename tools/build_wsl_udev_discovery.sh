#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr_udev_discovery.elf}"
src="${repo_root}/userland/fixtures/src/wsl_musl/lpr_udev_discovery.c"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
input_root="${repo_root}/.artifacts/userland-fixtures/alpine-input-root"
mesa_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"

[[ -d "${clang_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
[[ -e "${input_root}/usr/lib/libudev.so.1" ]] || bash "${repo_root}/tools/build_wsl_alpine_input.sh"
[[ -e "${mesa_root}/usr/lib/libdrm.so.2" ]] || bash "${repo_root}/tools/build_wsl_alpine_mesa.sh"
[[ -e "${runtime_libc}" ]] || bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"
libdrm_link="$(find "${mesa_root}/usr/lib" -maxdepth 1 -type f -name 'libdrm.so.2.*' | head -n 1)"
[[ -n "${libdrm_link}" ]] || { echo "versioned libdrm runtime not found" >&2; exit 1; }

out_abs="${repo_root}/${out}"
mkdir -p "$(dirname "${out_abs}")"
/usr/bin/clang -target x86_64-linux-musl --sysroot="${clang_root}" -std=c11 -O2 \
  "${src}" -nostdlib "${clang_root}/usr/lib/Scrt1.o" "${clang_root}/usr/lib/crti.o" \
  "${runtime_libc}" "${input_root}/usr/lib/libudev.so.1" \
  "${libdrm_link}" "${clang_root}/usr/lib/crtn.o" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -o "${out_abs}"
chmod 0755 "${out_abs}"
