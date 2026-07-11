#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${repo_root}/.artifacts/userland-fixtures/lpr_drm_prime_smoke.elf"
src="${repo_root}/userland/fixtures/src/wsl_musl/lpr_drm_prime_smoke.c"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
mesa_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-root"
mesa_dev_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-dev-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"

if [[ ! -d "${mesa_root}" || ! -d "${mesa_dev_root}" ]]; then
  bash "${repo_root}/tools/build_wsl_alpine_mesa.sh"
fi
if [[ ! -e "${runtime_libc}" ]]; then
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"
fi

obj="${out}.o"
gbm_lib="$(compgen -G "${mesa_root}/usr/lib/libgbm.so.*.*" | sort | tail -n 1)"
drm_lib="$(compgen -G "${mesa_root}/usr/lib/libdrm.so.*.*" | sort | tail -n 1)"
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" \
  -isystem "${mesa_dev_root}/usr/include" \
  -isystem "${mesa_dev_root}/usr/include/libdrm" \
  -std=c11 -O2 -fPIC -c "${src}" -o "${obj}"
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" -nostdlib \
  "${clang_root}/usr/lib/Scrt1.o" "${clang_root}/usr/lib/crti.o" "${obj}" \
  -L"${mesa_root}/usr/lib" -L"${clang_root}/usr/lib" \
  -Wl,-rpath-link,"${mesa_root}/usr/lib" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,--allow-shlib-undefined \
  -Wl,--no-as-needed "${gbm_lib}" "${drm_lib}" "${runtime_libc}" \
  "${clang_root}/usr/lib/crtn.o" -o "${out}"
rm -f "${obj}"
chmod 0755 "${out}"
