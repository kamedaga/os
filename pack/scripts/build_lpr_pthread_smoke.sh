#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out_base="${1:-.artifacts/userland-fixtures/lpr_pthread_smoke}"
src="${2:-${repo_root}/userland/fixtures/src/wsl_musl/lpr_pthread_smoke.c}"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"
sysroot="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"

if [[ ! -e "${sysroot}/usr/lib/Scrt1.o" ]]; then
  bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
fi
if [[ ! -e "${runtime_libc}" ]]; then
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"
fi

out_abs="${repo_root}/${out_base}"
mkdir -p "$(dirname "${out_abs}")"
obj="${out_abs}.o"
"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -std=c11 \
  -O2 \
  -fPIC \
  -pthread \
  -c "${src}" \
  -o "${obj}"

"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -nostdlib \
  -static \
  "${sysroot}/usr/lib/crt1.o" \
  "${sysroot}/usr/lib/crti.o" \
  "${obj}" \
  -L"${sysroot}/usr/lib" \
  -lc \
  "${sysroot}/usr/lib/crtn.o" \
  -o "${out_abs}.static.elf"

"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -nostdlib \
  "${sysroot}/usr/lib/Scrt1.o" \
  "${sysroot}/usr/lib/crti.o" \
  "${obj}" \
  -L"${sysroot}/usr/lib" \
  -L"${sysroot}/lib" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  "${runtime_libc}" \
  "${sysroot}/usr/lib/crtn.o" \
  -o "${out_abs}.dynamic.elf"

chmod 0755 "${out_abs}.static.elf" "${out_abs}.dynamic.elf"
rm -f "${obj}"

readelf -h "${out_abs}.static.elf" | grep -q 'Class:.*ELF64'
if readelf -l "${out_abs}.static.elf" | grep -q 'Requesting program interpreter'; then
  echo "static pthread smoke unexpectedly has an interpreter" >&2
  exit 1
fi
readelf -l "${out_abs}.dynamic.elf" | grep -q '/lib/ld-musl-x86_64.so.1'

printf 'built static and dynamic LPR musl fixtures at %s.{static,dynamic}.elf\n' "${out_abs}"
