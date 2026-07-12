#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr_wayland_shm_client.elf}"
src="${repo_root}/userland/fixtures/src/wsl_musl/lpr_wayland_shm_client.c"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
mesa_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-root"
sway_root="${repo_root}/.artifacts/userland-fixtures/alpine-sway-root"
sway_dev="${repo_root}/.artifacts/userland-fixtures/alpine-sway-dev-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"

[[ -d "${sway_root}" && -d "${sway_dev}" ]] || bash "${repo_root}/tools/build_wsl_alpine_sway.sh"
[[ -e "${runtime_libc}" ]] || bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"

work="${repo_root}/.artifacts/userland-fixtures/wayland-shm-client-build"
host_libs="${work}/host-libs"
generated="${work}/generated"
rm -rf "${work}"
mkdir -p "${host_libs}" "${generated}"

link_runtime_library() {
  local name="$1" target
  target="$(find "${clang_root}/usr/lib" "${mesa_root}/usr/lib" "${sway_root}/usr/lib" \
    -maxdepth 1 -type f -name "${name}.*" | sort | tail -n 1)"
  [[ -n "${target}" ]] || { echo "missing host scanner library ${name}" >&2; exit 1; }
  ln -sf "${target}" "${host_libs}/${name}"
}
for name in libexpat.so.1 libxml2.so.2 libz.so.1 liblzma.so.5; do
  link_runtime_library "${name}"
done

scanner="${sway_dev}/usr/bin/wayland-scanner"
protocol="${sway_dev}/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
LD_LIBRARY_PATH="${host_libs}" "${scanner}" client-header "${protocol}" "${generated}/xdg-shell-client-protocol.h"
LD_LIBRARY_PATH="${host_libs}" "${scanner}" private-code "${protocol}" "${generated}/xdg-shell-protocol.c"

out_abs="${repo_root}/${out}"
client_obj="${work}/client.o"
protocol_obj="${work}/protocol.o"
mkdir -p "$(dirname "${out_abs}")"
common=(-target x86_64-linux-musl --sysroot="${clang_root}" -isystem "${sway_dev}/usr/include" -I"${generated}" -std=c11 -O2 -fPIC)
"${cc}" "${common[@]}" -c "${src}" -o "${client_obj}"
"${cc}" "${common[@]}" -c "${generated}/xdg-shell-protocol.c" -o "${protocol_obj}"
wayland_client="$(find "${mesa_root}/usr/lib" "${sway_root}/usr/lib" -maxdepth 1 -type f -name 'libwayland-client.so.*.*' | sort | tail -n 1)"
[[ -n "${wayland_client}" ]] || { echo "missing Wayland client library" >&2; exit 1; }
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" -nostdlib \
  "${clang_root}/usr/lib/Scrt1.o" "${clang_root}/usr/lib/crti.o" \
  "${client_obj}" "${protocol_obj}" "${wayland_client}" "${runtime_libc}" \
  "${clang_root}/usr/lib/crtn.o" -Wl,--allow-shlib-undefined \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -o "${out_abs}"
chmod 0755 "${out_abs}"
readelf -d "${out_abs}" | grep -q 'libwayland-client.so.0'
printf 'built wl_shm Wayland client fixture at %s\n' "${out_abs}"
