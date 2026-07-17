#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/pacha-user-session}"
src="${repo_root}/userland/pacha_user_session/src/main.c"
input_metadata_src="${repo_root}/userland/pacha_user_session/src/input_metadata.c"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
cc="/usr/bin/clang"

[[ -x "${cc}" ]] || { echo "missing ${cc}" >&2; exit 1; }
[[ -d "${clang_root}" ]] || { echo "missing ${clang_root}" >&2; exit 1; }
[[ -e "${runtime_libc}" ]] ||
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"

case "${out}" in
  /*) out_abs="${out}" ;;
  *) out_abs="${repo_root}/${out}" ;;
esac
mkdir -p "$(dirname "${out_abs}")"

"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" -std=c11 -O2 \
  -Wall -Wextra "${src}" "${input_metadata_src}" -nostdlib \
  "${clang_root}/usr/lib/Scrt1.o" "${clang_root}/usr/lib/crti.o" \
  "${runtime_libc}" "${clang_root}/usr/lib/crtn.o" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -o "${out_abs}"

chmod 0755 "${out_abs}"
readelf -l "${out_abs}" | grep -q '/lib/ld-musl-x86_64.so.1'
printf 'built pacha user session at %s\n' "${out_abs}"
