#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/alpine-sway-root}"
dev_out="${2:-.artifacts/userland-fixtures/alpine-sway-dev-root}"
branch="${ALPINE_SWAY_VERSION:-v3.22}"
arch="${ALPINE_SWAY_ARCH:-x86_64}"
mirror="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
cache="${repo_root}/.artifacts/third_party/alpine-sway-${branch}-${arch}"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
mesa_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-root"
mesa_dev_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-dev-root"
input_root="${repo_root}/.artifacts/userland-fixtures/alpine-input-root"
input_dev_root="${repo_root}/.artifacts/userland-fixtures/alpine-input-dev-root"
linux_musl="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
out_abs="${repo_root}/${out}"
dev_out_abs="${repo_root}/${dev_out}"
wlroots_commit="cda69b696d65a53d5d5e75dfed059a3803e0d700"
build_tools="${repo_root}/.artifacts/third_party/wlroots-build-tools"

[[ -d "${clang_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
[[ -d "${mesa_root}" && -d "${mesa_dev_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_mesa.sh"
[[ -d "${input_root}" && -d "${input_dev_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_input.sh"
[[ -f "${linux_musl}" ]] ||
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"
mkdir -p "${cache}"
tmp="$(mktemp -d "${cache}/extract.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT

for section in main community; do
  curl -fsSL "${mirror}/${branch}/${section}/${arch}/APKINDEX.tar.gz" \
    -o "${cache}/${section}-index.tar.gz"
  tar -xOzf "${cache}/${section}-index.tar.gz" APKINDEX >"${cache}/${section}-index"
done

field_in() {
  awk -v want="$2" -v field="$3" '
    BEGIN { RS=""; FS="\n" }
    {
      package=0
      for (i=1; i<=NF; ++i) if ($i == "P:" want) package=1
      if (package) for (i=1; i<=NF; ++i)
        if (index($i, field ":") == 1) {
          print substr($i, length(field) + 2)
          exit
        }
    }
  ' "$1"
}

section_for() {
  local section
  for section in main community; do
    if [[ -n "$(field_in "${cache}/${section}-index" "$1" V)" ]]; then
      printf '%s\n' "${section}"
      return 0
    fi
  done
  return 1
}

field() {
  local section
  section="$(section_for "$1")" || return 1
  field_in "${cache}/${section}-index" "$1" "$2"
}

normalize() {
  local dependency="${1#!}"
  printf '%s\n' "${dependency%%[<>=~]*}"
}

provider() {
  local dependency="$1" section found
  case "${dependency}" in
    so:libudev.so.1|eudev-libs|udev)
      printf '%s\n' libudev-zero
      return 0
      ;;
  esac
  if section_for "${dependency}" >/dev/null 2>&1; then
    printf '%s\n' "${dependency}"
    return 0
  fi
  for section in main community; do
    found="$(awk -v want="${dependency}" '
      BEGIN { RS=""; FS="\n" }
      {
        package=""
        for (i=1; i<=NF; ++i) if (index($i, "P:") == 1) package=substr($i, 3)
        for (i=1; i<=NF; ++i) if (index($i, "p:") == 1) {
          count=split(substr($i, 3), provides, " ")
          for (j=1; j<=count; ++j) {
            sub(/[<>=~].*/, "", provides[j])
            if (provides[j] == want) { print package; exit }
          }
        }
      }
    ' "${cache}/${section}-index")"
    if [[ -n "${found}" ]]; then
      printf '%s\n' "${found}"
      return 0
    fi
  done
  return 1
}

declare -A queued=() wanted=()
queue=()
enqueue() {
  local dependency="$1" package
  case "${dependency}" in
    ""|libc.musl-*|so:libc.musl-*|busybox|busybox-binsh|cmd:sh|/bin/sh|pkgconfig|cmd:pkg-config)
      return
      ;;
  esac
  package="$(provider "${dependency}")" || {
    echo "failed to resolve Alpine Sway dependency ${dependency}" >&2
    exit 1
  }
  case "${package}" in
    busybox|busybox-binsh|musl|libudev-zero|seatd|seatd-launch|libseat|libinput|libinput-libs|libinput-udev|libevdev|mtdev)
      # The input overlay already publishes these packages. In particular its
      # libseat is the seatd-only build required by PachaOS.
      return
      ;;
  esac
  if [[ -z "${queued[${package}]+x}" ]]; then
    queued["${package}"]=1
    queue+=("${package}")
  fi
}

for package in sway swaybar swaybg swaynag sway-wallpapers wlroots wayland wayland-protocols wayland-utils foot font-roboto-mono libxkbcommon pixman hwdata-pnp; do
  enqueue "${package}"
done
for ((i=0; i<${#queue[@]}; ++i)); do
  package="${queue[$i]}"
  wanted["${package}"]=1
  for dependency in $(field "${package}" D || true); do
    enqueue "$(normalize "${dependency}")"
  done
done

download() {
  local package="$1" version section apk
  version="$(field "${package}" V)"
  section="$(section_for "${package}")"
  apk="${cache}/${package}-${version}.apk"
  if [[ ! -f "${apk}" ]]; then
    curl -fsSL "${mirror}/${branch}/${section}/${arch}/${package}-${version}.apk" -o "${apk}"
  fi
  printf '%s\n' "${apk}"
}

runtime="${tmp}/runtime"
dev="${tmp}/dev"
mkdir -p "${runtime}" "${dev}"
for package in "${!wanted[@]}"; do
  tar --warning=no-unknown-keyword -xzf "$(download "${package}")" -C "${runtime}"
done
for package in \
  wayland-dev wayland-protocols libffi-dev libxkbcommon-dev pixman-dev \
  libdisplay-info-dev hwdata-dev libxcb-dev xcb-util-wm-dev \
  xcb-util-renderutil-dev xcb-util-dev xcb-proto xorgproto \
  libxau-dev libxdmcp-dev; do
  tar --warning=no-unknown-keyword -xzf "$(download "${package}")" -C "${dev}"
done

rm -rf \
  "${runtime}"/.SIGN.* "${runtime}"/.PKGINFO "${runtime}"/.pre-* "${runtime}"/.post-* \
  "${runtime}"/etc/init.d "${runtime}"/etc/conf.d "${runtime}"/var \
  "${dev}"/.SIGN.* "${dev}"/.PKGINFO "${dev}"/.pre-* "${dev}"/.post-* \
  "${dev}"/etc "${dev}"/var

# APK triggers normally create this cache.  The fixture extracts APKs without
# running their scripts, so build the target-root cache explicitly while the
# fontconfig symlinks still have their native representation.
mkdir -p "${runtime}/var/cache/fontconfig"
host_fc_cache=/usr/bin/fc-cache
[[ -x "${host_fc_cache}" ]] || { echo "missing host ${host_fc_cache}" >&2; exit 1; }
# Nix's 1980 SOURCE_DATE_EPOCH makes fontconfig immediately reject its own
# cache as older than the APK-preserved font directory mtimes.
env -u SOURCE_DATE_EPOCH "${host_fc_cache}" \
  --really-force --system-only --sysroot="${runtime}" \
  /usr/share/fonts >/dev/null
find "${runtime}/var/cache/fontconfig" -maxdepth 1 -type f -name '*cache-*' -print -quit |
  grep -q . || { echo "host fc-cache did not populate target root" >&2; exit 1; }

# This image intentionally has no Xwayland server. Use Alt for the guest
# modifier and disable the unavailable optional backend.
sed -i 's/^set \$mod Mod4$/set $mod Alt/' "${runtime}/etc/sway/config"
grep -Fxq 'set $mod Alt' "${runtime}/etc/sway/config" || {
  echo "failed to select Alt as the Sway modifier" >&2
  exit 1
}
mkdir -p "${runtime}/etc/sway/config.d"
printf 'xwayland disable\n' >"${runtime}/etc/sway/config.d/pacha.conf"

# Build wlroots itself so the product path contains the generic fence and
# sealed-memfd changes. Sway remains the unmodified Alpine executable and
# resolves this ABI-compatible shared object at runtime.
if [[ ! -x "${build_tools}/bin/meson" || ! -x "${build_tools}/bin/ninja" ]]; then
  rm -rf "${build_tools}"
  python3 -m venv "${build_tools}"
  "${build_tools}/bin/pip" install --disable-pip-version-check \
    meson==1.8.1 ninja==1.11.1.4
fi
tool_path="${build_tools}/bin:${PATH}"

wayland_archive="${cache}/wayland-1.23.1.tar.xz"
wayland_source="${cache}/wayland-1.23.1"
wayland_build="${cache}/wayland-host-build"
[[ -f "${wayland_archive}" ]] || curl -fsSL \
  'https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.23.1/downloads/wayland-1.23.1.tar.xz' \
  -o "${wayland_archive}"
if [[ ! -f "${wayland_source}/meson.build" ]]; then
  rm -rf "${wayland_source}"
  mkdir -p "${wayland_source}"
  tar -xJf "${wayland_archive}" --strip-components=1 -C "${wayland_source}"
fi
if [[ ! -x "${wayland_build}/src/wayland-scanner" ]]; then
  rm -rf "${wayland_build}"
  PATH="${tool_path}" CC=/usr/bin/clang "${build_tools}/bin/meson" setup \
    "${wayland_build}" "${wayland_source}" \
    -Ddocumentation=false -Dtests=false -Ddtd_validation=false \
    -Dlibraries=false -Dscanner=true
  "${build_tools}/bin/ninja" -C "${wayland_build}"
fi

wlroots_git="${cache}/wlroots-git"
if [[ ! -d "${wlroots_git}/.git" ]]; then
  rm -rf "${wlroots_git}"
  git clone --filter=blob:none --no-checkout \
    https://gitlab.freedesktop.org/wlroots/wlroots.git "${wlroots_git}"
fi
if ! git -C "${wlroots_git}" cat-file -e "${wlroots_commit}^{commit}" 2>/dev/null; then
  git -C "${wlroots_git}" fetch --depth=1 origin "${wlroots_commit}"
fi
wlroots_source="${tmp}/wlroots-source"
mkdir -p "${wlroots_source}"
git -C "${wlroots_git}" archive "${wlroots_commit}" | tar -x -C "${wlroots_source}"
GIT_CEILING_DIRECTORIES="${repo_root}/.artifacts" git -C "${wlroots_source}" apply \
  "${repo_root}/pack/patches/wlroots/0001-use-memfd-for-shm-files.patch" \
  "${repo_root}/pack/patches/wlroots/0002-explicit-render-fences.patch"
/usr/bin/grep -Fq 'memfd_create("wlroots-shm"' "${wlroots_source}/util/shm.c" || {
  echo "wlroots sealed-memfd patch was not applied to the extracted source" >&2
  exit 1
}
/usr/bin/grep -Fq 'wlr_buffer_set_acquire_fence' "${wlroots_source}/render/gles2/pass.c" || {
  echo "wlroots render-fence patch was not applied to the extracted source" >&2
  exit 1
}

cross_root="${tmp}/cross-root"
python3 - "${cross_root}" \
  "${clang_root}" "${mesa_root}" "${input_root}" "${runtime}" \
  "${mesa_dev_root}" "${input_dev_root}" "${dev}" <<'PY'
import os
import sys
from pathlib import Path

destination = Path(sys.argv[1]).resolve()
roots = [Path(argument).resolve() for argument in sys.argv[2:]]
marker = b"CAPABILITYOS_ROOTFS_SYMLINK\n"
destination.mkdir(parents=True)

for root in roots:
    for source in sorted(root.rglob("*"), key=lambda path: (len(path.parts), str(path))):
        relative = source.relative_to(root)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            if target.exists() or target.is_symlink():
                if target.is_dir() and not target.is_symlink():
                    continue
                target.unlink()
            target.symlink_to(os.readlink(source))
        elif source.is_dir():
            if target.is_symlink() or (target.exists() and not target.is_dir()):
                raise SystemExit(f"cross-root directory collision: {relative}")
            target.mkdir(exist_ok=True)
        elif source.is_file():
            if target.exists() or target.is_symlink():
                if target.is_dir() and not target.is_symlink():
                    raise SystemExit(f"cross-root file collision: {relative}")
                target.unlink()
            payload = source.read_bytes()
            if payload.startswith(marker):
                target.symlink_to(payload[len(marker):].decode())
            else:
                target.symlink_to(source)
PY
install -Dm0755 "${linux_musl}" "${cross_root}/lib/ld-musl-x86_64.so.1"
rm -f "${cross_root}/usr/lib/libudev.so"
ln -s libudev.so.1 "${cross_root}/usr/lib/libudev.so"

native_pc="${tmp}/native-pc"
mkdir -p "${native_pc}"
printf '%s\n' \
  'Name: Wayland Scanner' \
  'Description: native Wayland protocol scanner' \
  'Version: 1.23.1' \
  "wayland_scanner=${wayland_build}/src/wayland-scanner" \
  >"${native_pc}/wayland-scanner.pc"
printf '%s\n' \
  'Name: hwdata' \
  'Description: native hardware identification data' \
  'Version: 0.395' \
  "pkgdatadir=${cross_root}/usr/share/hwdata" \
  >"${native_pc}/hwdata.pc"

# Alpine's generic egl.pc lists private X11 dependencies even when consumers
# dynamically link libEGL and the compositor's X11 backend is disabled. Keep
# the target package version/paths while exposing only the dependency used by
# this headless DRM build.
cross_pc="${tmp}/cross-pc"
mkdir -p "${cross_pc}"
printf '%s\n' \
  'prefix=/usr' \
  'includedir=${prefix}/include' \
  'libdir=${prefix}/lib' \
  'Name: egl' \
  'Description: Mesa EGL library for the DRM-only wlroots build' \
  'Version: 25.1.9' \
  'Requires.private: libdrm >= 2.4.75' \
  'Libs: -L${libdir} -lEGL' \
  'Libs.private: -lpthread -pthread -lm' \
  'Cflags: -I${includedir}' \
  >"${cross_pc}/egl.pc"
printf '%s\n' \
  'prefix=/usr' \
  'includedir=${prefix}/include' \
  'libdir=${prefix}/lib' \
  'have_seatd=true' \
  'have_logind=false' \
  'have_builtin=false' \
  'Name: libseat' \
  'Description: seatd-only seat management library' \
  'Version: 0.9.1' \
  'Libs: -L${libdir} -lseat' \
  'Libs.private: -lrt' \
  'Cflags: -I${includedir}' \
  >"${cross_pc}/libseat.pc"
printf '%s\n' \
  'Name: xwayland' \
  'Description: Xwayland executable contract' \
  'Version: 24.1.6' \
  'xwayland=/usr/bin/Xwayland' \
  'have_listenfd=true' \
  'have_no_touch_pointer_emulation=true' \
  'have_force_xrandr_emulation=true' \
  'have_terminate_delay=true' \
  >"${cross_pc}/xwayland.pc"

cross_pkg_config="${tmp}/cross-pkg-config"
printf '%s\n' \
  '#!/bin/sh' \
  "export PKG_CONFIG_SYSROOT_DIR='${cross_root}'" \
  "export PKG_CONFIG_LIBDIR='${cross_pc}:${cross_root}/usr/lib/pkgconfig:${cross_root}/usr/share/pkgconfig'" \
  'unset PKG_CONFIG_PATH' \
  'exec /usr/bin/pkg-config "$@"' \
  >"${cross_pkg_config}"
chmod 0755 "${cross_pkg_config}"

cross_file="${tmp}/wlroots-cross.ini"
printf '%s\n' \
  '[binaries]' \
  "c = ['/usr/bin/clang', '--target=x86_64-linux-musl', '--sysroot=${cross_root}']" \
  "ar = '/usr/bin/ar'" \
  "strip = '/usr/bin/strip'" \
  "pkg-config = '${cross_pkg_config}'" \
  '[properties]' \
  'needs_exe_wrapper = true' \
  '[host_machine]' \
  "system = 'linux'" \
  "cpu_family = 'x86_64'" \
  "cpu = 'x86_64'" \
  "endian = 'little'" \
  >"${cross_file}"

wlroots_build="${tmp}/wlroots-build"
PKG_CONFIG_PATH="${native_pc}" PKG_CONFIG_PATH_FOR_BUILD="${native_pc}" \
  PATH="${tool_path}" \
  "${build_tools}/bin/meson" setup "${wlroots_build}" "${wlroots_source}" \
  --cross-file "${cross_file}" --prefix=/usr --buildtype=release \
  -Dauto_features=disabled -Ddefault_library=shared \
  -Dbackends=drm,libinput,x11 -Drenderers=gles2 -Dallocators=gbm \
  -Dsession=enabled -Dexamples=false -Dxwayland=enabled \
  -Dcolor-management=disabled -Dlibliftoff=disabled -Dxcb-errors=disabled
"${build_tools}/bin/ninja" -C "${wlroots_build}"
wlroots_install="${tmp}/wlroots-install"
DESTDIR="${wlroots_install}" "${build_tools}/bin/ninja" -C "${wlroots_build}" install
install -m 0755 "${wlroots_install}/usr/lib/libwlroots-0.18.so" \
  "${runtime}/usr/lib/libwlroots-0.18.so"
/usr/bin/strip --strip-unneeded "${runtime}/usr/lib/libwlroots-0.18.so"
/usr/bin/readelf -Ws "${runtime}/usr/lib/libwlroots-0.18.so" |
  /usr/bin/grep -E '[[:space:]]UND[[:space:]]+memfd_create(@|$)' >/dev/null || {
    echo "built wlroots does not use the sealed-memfd shared-file path" >&2
    exit 1
  }

python3 - "${runtime}" "${clang_root}" "${mesa_root}" "${input_root}" <<'PY'
import os
import sys
from pathlib import Path

runtime = Path(sys.argv[1]).resolve()
shared_roots = [Path(argument).resolve() for argument in sys.argv[2:]]

for link in sorted(path for path in runtime.rglob("*") if path.is_symlink()):
    target = os.readlink(link)
    resolved = (runtime / target.lstrip("/")) if target.startswith("/") else (link.parent / target)
    resolved = resolved.resolve(strict=False)
    if resolved != runtime and runtime not in resolved.parents:
        raise SystemExit(f"symlink escapes root: {link} -> {target}")
    link.unlink()
    link.write_bytes(b"CAPABILITYOS_ROOTFS_SYMLINK\n" + target.encode())

for path in sorted(path for path in runtime.rglob("*") if path.is_file()):
    relative = path.relative_to(runtime)
    for shared_root in shared_roots:
        shared = shared_root / relative
        if not shared.is_file():
            continue
        if path.read_bytes() != shared.read_bytes():
            raise SystemExit(f"Sway/shared rootfs collision differs: /{relative}")
        path.unlink()
        break

for directory in sorted((path for path in runtime.rglob("*") if path.is_dir()), reverse=True):
    try:
        directory.rmdir()
    except OSError:
        pass
PY

for required in \
  usr/bin/sway \
  usr/bin/swaymsg \
  usr/bin/wayland-info \
  usr/bin/foot \
  'usr/share/fonts/roboto-mono/RobotoMono[wght].ttf' \
  usr/lib/libwlroots-0.18.so \
  usr/lib/libwayland-client.so.0 \
  usr/lib/libwayland-server.so.0 \
  usr/lib/libxkbcommon.so.0 \
  usr/lib/libpixman-1.so.0 \
  usr/share/X11/xkb/rules/evdev; do
  if [[ ! -e "${runtime}/${required}" && ! -e "${mesa_root}/${required}" && ! -e "${input_root}/${required}" ]]; then
    echo "missing Sway runtime /${required}" >&2
    exit 1
  fi
done
fontconfig_cache="$(find "${runtime}/var/cache/fontconfig" -maxdepth 1 -type f -name '*cache-*' -print -quit)"
if [[ -z "${fontconfig_cache}" ]]; then
  echo "missing Sway runtime fontconfig cache" >&2
  exit 1
fi
for required in usr/include/wayland-client.h usr/bin/wayland-scanner usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml; do
  [[ -e "${dev}/${required}" ]] || { echo "missing Sway fixture development /${required}" >&2; exit 1; }
done

rm -rf "${out_abs}.tmp" "${dev_out_abs}.tmp" "${out_abs}" "${dev_out_abs}"
mkdir -p "$(dirname "${out_abs}")" "$(dirname "${dev_out_abs}")"
mv "${runtime}" "${out_abs}.tmp"
mv "${dev}" "${dev_out_abs}.tmp"
mv "${out_abs}.tmp" "${out_abs}"
mv "${dev_out_abs}.tmp" "${dev_out_abs}"

printf 'built Alpine Sway runtime into %s\n' "${out_abs}"
printf 'runtime packages:'
printf ' %s' "${!wanted[@]}"
printf '\ninput-overlay packages: seatd seatd-launch libseat libinput libinput-libs libinput-udev libevdev mtdev libudev-zero\n'
du -sh "${out_abs}" "${dev_out_abs}"
