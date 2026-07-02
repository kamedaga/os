#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-dyn-needed-root}"
cache="${repo_root}/.artifacts/third_party/alpine-lua-cli"
sysroot="${cache}/alpine-sysroot"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"
out_abs="${repo_root}/${out}"

if [[ ! -e "${repo_root}/.artifacts/userland/lpr_linux_musl_libc/LPRMUSL.SO" ]]; then
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"
fi
if [[ ! -e "${sysroot}/usr/lib/Scrt1.o" ]]; then
  bash "${repo_root}/tools/build_lpr_lua_launcher.sh" ".artifacts/userland-fixtures/lpr_lua_launcher.elf"
fi

rm -rf "${out_abs}"
mkdir -p "${out_abs}/cmd" "${out_abs}/usr/lib"

"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -fPIC \
  -shared \
  -nostdlib \
  "${repo_root}/userland/fixtures/linux/lpr_dyn_needed_lib.c" \
  -Wl,-soname,liblprneed.so \
  -o "${out_abs}/usr/lib/liblprneed.so"

obj="${out_abs}/lpr_dyn_needed_main.o"
"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -fPIC \
  -c \
  "${repo_root}/userland/fixtures/linux/lpr_dyn_needed_main.c" \
  -o "${obj}"

"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -nostdlib \
  "${sysroot}/usr/lib/Scrt1.o" \
  "${sysroot}/usr/lib/crti.o" \
  "${obj}" \
  -L"${out_abs}/usr/lib" \
  -L"${sysroot}/usr/lib" \
  -L"${sysroot}/lib" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  -Wl,--allow-shlib-undefined \
  -l:liblprneed.so \
  -lc \
  "${sysroot}/usr/lib/crtn.o" \
  -o "${out_abs}/cmd/lpr_dyn_needed.elf"

chmod 0755 "${out_abs}/cmd/lpr_dyn_needed.elf" "${out_abs}/usr/lib/liblprneed.so"
