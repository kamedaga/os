#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr_mesa_inventory.elf}"
src="${repo_root}/${2:-userland/fixtures/src/wsl_musl/lpr_mesa_inventory.c}"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
mesa_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-root"
mesa_dev_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-dev-root"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"

if [[ ! -d "${mesa_root}" || ! -d "${mesa_dev_root}" ]]; then
  bash "${repo_root}/tools/build_wsl_alpine_mesa.sh"
fi
if [[ ! -e "${runtime_libc}" ]]; then
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"
fi

out_abs="${repo_root}/${out}"
mkdir -p "$(dirname "${out_abs}")"
obj="${out_abs}.o"
egl_lib="$(compgen -G "${mesa_root}/usr/lib/libEGL.so.*.*" | sort | tail -n 1)"
gles_lib="$(compgen -G "${mesa_root}/usr/lib/libGLESv2.so.*.*" | sort | tail -n 1)"
gbm_lib="$(compgen -G "${mesa_root}/usr/lib/libgbm.so.*.*" | sort | tail -n 1)"
drm_lib="$(compgen -G "${mesa_root}/usr/lib/libdrm.so.*.*" | sort | tail -n 1)"
for library in "${egl_lib}" "${gles_lib}" "${gbm_lib}" "${drm_lib}"; do
  if [[ ! -f "${library}" ]]; then
    echo "missing Mesa fixture link library" >&2
    exit 1
  fi
done

"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${clang_root}" \
  -isystem "${mesa_dev_root}/usr/include" \
  -isystem "${mesa_dev_root}/usr/include/libdrm" \
  -std=c11 -O2 -fPIC \
  -c "${src}" -o "${obj}"

"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${clang_root}" \
  -nostdlib \
  "${clang_root}/usr/lib/Scrt1.o" \
  "${clang_root}/usr/lib/crti.o" \
  "${obj}" \
  -L"${mesa_root}/usr/lib" \
  -L"${clang_root}/usr/lib" \
  -Wl,-rpath-link,"${mesa_root}/usr/lib" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  -Wl,--allow-shlib-undefined \
  -Wl,--no-as-needed \
  "${egl_lib}" "${gles_lib}" "${gbm_lib}" "${drm_lib}" \
  "${runtime_libc}" \
  "${clang_root}/usr/lib/crtn.o" \
  -o "${out_abs}"

rm -f "${obj}"
chmod 0755 "${out_abs}"
readelf -l "${out_abs}" | grep -q '/lib/ld-musl-x86_64.so.1'
for library in libEGL.so.1 libGLESv2.so.2 libgbm.so.1 libdrm.so.2; do
  readelf -d "${out_abs}" | grep -q "Shared library: \[${library}\]"
done
printf 'built Mesa inventory fixture at %s\n' "${out_abs}"
