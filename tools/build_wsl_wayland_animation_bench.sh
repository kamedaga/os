#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr_wayland_animation_bench.elf}"
src="${repo_root}/userland/fixtures/src/wsl_musl/lpr_wayland_animation_bench.c"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
mesa_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-root"
sway_root="${repo_root}/.artifacts/userland-fixtures/alpine-sway-root"
sway_dev="${repo_root}/.artifacts/userland-fixtures/alpine-sway-dev-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"

[[ -d "${sway_root}" && -d "${sway_dev}" ]] || bash "${repo_root}/tools/build_wsl_alpine_sway.sh"
[[ -e "${runtime_libc}" ]] || bash "${repo_root}/tools/copy_lpr_linux_musl.sh" \
  ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"

work="${repo_root}/.artifacts/userland-fixtures/wayland-animation-bench-build"
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
xdg="${sway_dev}/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
presentation="${sway_dev}/usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml"
for protocol in xdg-shell presentation-time; do
  case "${protocol}" in
    xdg-shell) xml="${xdg}" ;;
    presentation-time) xml="${presentation}" ;;
  esac
  LD_LIBRARY_PATH="${host_libs}" "${scanner}" client-header "${xml}" \
    "${generated}/${protocol}-client-protocol.h"
  LD_LIBRARY_PATH="${host_libs}" "${scanner}" private-code "${xml}" \
    "${generated}/${protocol}-protocol.c"
done

out_abs="${repo_root}/${out}"
mkdir -p "$(dirname "${out_abs}")"
common=(-target x86_64-linux-musl --sysroot="${clang_root}" \
  -isystem "${sway_dev}/usr/include" -I"${generated}" -std=c11 -O2 -fPIC \
  -Wall -Wextra -Werror)
objects=()
for source in "${src}" "${generated}/xdg-shell-protocol.c" \
  "${generated}/presentation-time-protocol.c"; do
  object="${work}/$(basename "${source}" .c).o"
  "${cc}" "${common[@]}" -c "${source}" -o "${object}"
  objects+=("${object}")
done

wayland_client="$(find "${mesa_root}/usr/lib" "${sway_root}/usr/lib" \
  -maxdepth 1 -type f -name 'libwayland-client.so.*.*' | sort | tail -n 1)"
[[ -n "${wayland_client}" ]] || { echo "missing Wayland client library" >&2; exit 1; }
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" -nostdlib \
  "${clang_root}/usr/lib/Scrt1.o" "${clang_root}/usr/lib/crti.o" \
  "${objects[@]}" "${wayland_client}" "${runtime_libc}" \
  "${clang_root}/usr/lib/crtn.o" -Wl,--allow-shlib-undefined \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -o "${out_abs}"
chmod 0755 "${out_abs}"
readelf -d "${out_abs}" | grep -q 'libwayland-client.so.0'
printf 'built Wayland animation benchmark at %s\n' "${out_abs}"
