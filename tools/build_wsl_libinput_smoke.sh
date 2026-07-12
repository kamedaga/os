#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr_libinput_seatd_smoke.elf}"
src="${repo_root}/${2:-userland/fixtures/src/wsl_musl/lpr_libinput_seatd_smoke.c}"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
input_root="${repo_root}/.artifacts/userland-fixtures/alpine-input-root"
input_dev="${repo_root}/.artifacts/userland-fixtures/alpine-input-dev-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"
[[ -d "${input_root}" && -d "${input_dev}" ]] || bash "${repo_root}/tools/build_wsl_alpine_input.sh"
[[ -e "${runtime_libc}" ]] || bash "${repo_root}/tools/copy_lpr_linux_musl.sh" .artifacts/userland-fixtures/lpr-linux-musl-libc.so

out_abs="${repo_root}/${out}"; mkdir -p "$(dirname "${out_abs}")"
obj="${out_abs}.o"
libinput="$(compgen -G "${input_root}/usr/lib/libinput.so.*.*" | sort | tail -n1)"
libseat="${input_root}/usr/lib/libseat.so.1"
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" \
  -isystem "${input_dev}/usr/include" -isystem "${input_dev}/usr/include/libevdev-1.0" \
  -std=c11 -O2 -fPIC -c "${src}" -o "${obj}"
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" -nostdlib \
  "${clang_root}/usr/lib/Scrt1.o" "${clang_root}/usr/lib/crti.o" "${obj}" \
  -L"${input_root}/usr/lib" -L"${clang_root}/usr/lib" \
  -Wl,-rpath-link,"${input_root}/usr/lib" -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  -Wl,--allow-shlib-undefined -Wl,--no-as-needed "${libinput}" "${libseat}" \
  "${runtime_libc}" "${clang_root}/usr/lib/crtn.o" -o "${out_abs}"
rm -f "${obj}"; chmod 0755 "${out_abs}"
readelf -d "${out_abs}" | grep -q 'libinput.so.10'
readelf -d "${out_abs}" | grep -q 'libseat.so.1'
printf 'built libinput+seatd smoke fixture at %s\n' "${out_abs}"
