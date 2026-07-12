#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr_sway_launcher.elf}"
src="${repo_root}/userland/fixtures/src/wsl_musl/lpr_sway_launcher.c"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"

[[ -d "${clang_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
[[ -e "${runtime_libc}" ]] || bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"

out_abs="${repo_root}/${out}"
mkdir -p "$(dirname "${out_abs}")"
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" -std=c11 -O2 \
  "${src}" -nostdlib "${clang_root}/usr/lib/Scrt1.o" "${clang_root}/usr/lib/crti.o" \
  "${runtime_libc}" "${clang_root}/usr/lib/crtn.o" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -o "${out_abs}"
chmod 0755 "${out_abs}"
readelf -l "${out_abs}" | grep -q '/lib/ld-musl-x86_64.so.1'
printf 'built Sway launcher fixture at %s\n' "${out_abs}"
