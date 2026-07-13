#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/libm57-wlroots-keymap-compat.so}"
src="${repo_root}/userland/fixtures/src/wsl_musl/lpr_wlroots_keymap_compat.c"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"

[[ -d "${clang_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
[[ -e "${runtime_libc}" ]] || bash "${repo_root}/tools/copy_lpr_linux_musl.sh" \
  ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"

out_abs="${repo_root}/${out}"
mkdir -p "$(dirname "${out_abs}")"
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" \
  -std=c11 -O2 -fPIC -shared -nostdlib \
  "${src}" "${runtime_libc}" -Wl,--allow-shlib-undefined \
  -Wl,-soname,libm57-wlroots-keymap-compat.so -o "${out_abs}"
chmod 0755 "${out_abs}"
readelf -Ws "${out_abs}" | grep -q ' shm_open$'
readelf -Ws "${out_abs}" | grep -q ' shm_unlink$'
printf 'built wlroots keymap memfd compatibility library at %s\n' "${out_abs}"
