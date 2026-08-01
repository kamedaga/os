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
stock_sway_branch="v3.23"
stock_sway_version="1.11-r2"
stock_sway_sha256="1483f7ae56a415a1baca6774c04b73d3406242d687358aad189dac2ff9822a74"
stock_wlroots_version="0.19.2-r0"
stock_wlroots_sha256="63449bd77d9c14fe3231004f652c4b1af0e792249f8ca516a64efa4ae9d75aed"
stock_display_info_version="0.3.0-r0"
stock_display_info_sha256="a8796066ec54f870927657948264b318b27cdbd0451469a7d9d46c73254d06df"
stock_liftoff_branch="v3.22"
stock_liftoff_version="0.5.0-r0"
stock_liftoff_sha256="454dba3b7e4e109ee89ba4be65a30f742059e4b578c4fb5dcae9d9ccb2252b39"
ipaex_source_url="https://data.wolfsden.cz/mirror/IPAexfont00401.zip"
ipaex_source_sha512="fe639ded0a25eed66df8cc1e9d5e965b501574a25fab542a749b3cb8464690448e44343ff5845aecd3224ec481c4089ee56e64880cbbc9211a260b22d4cc68cd"
adwaita_source_url="https://download.gnome.org/sources/adwaita-icon-theme/48/adwaita-icon-theme-48.1.tar.xz"
adwaita_source_sha512="1d116599d5397a9dbc7e580fe78ba675b2d6e055e2c6387c08b4f8a646e989a4e5b04a6ff0d8d357422ea7100aefc54c568abf37251f7927ae081ac4334742db"

[[ "${branch}" == "v3.22" ]] || {
  echo "the stock Sway 1.11 overlay requires an Alpine v3.22 base, got ${branch}" >&2
  exit 1
}
[[ "${arch}" == "x86_64" ]] || {
  echo "the pinned stock Sway overlay is only verified for x86_64, got ${arch}" >&2
  exit 1
}

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

for package in sway swaybar swaybg swaynag sway-wallpapers wlroots wayland wayland-protocols wayland-utils foot gtk+3.0 gtk+3.0-demo adwaita-icon-theme font-ipaex font-roboto-mono libxkbcommon pixman hwdata-pnp; do
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

download_pinned() {
  local apk_branch="$1" section="$2" package="$3" version="$4" expected_sha256="$5"
  local apk staged actual_sha256
  apk="${cache}/${apk_branch}-${package}-${version}.apk"
  if [[ -f "${apk}" ]]; then
    actual_sha256="$(sha256sum "${apk}" | awk '{print $1}')"
  else
    actual_sha256=""
  fi
  if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
    staged="${tmp}/${package}-${version}.apk.download"
    curl -fsSL \
      "${mirror}/${apk_branch}/${section}/${arch}/${package}-${version}.apk" \
      -o "${staged}"
    actual_sha256="$(sha256sum "${staged}" | awk '{print $1}')"
    if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
      echo "pinned Alpine APK checksum mismatch: ${package}-${version}" >&2
      echo "expected ${expected_sha256}, got ${actual_sha256}" >&2
      exit 1
    fi
    mv -f "${staged}" "${apk}"
  fi
  printf '%s\n' "${apk}"
}

download_source_pinned() {
  local name="$1" url="$2" expected_sha512="$3"
  local archive="${cache}/${name}" staged actual_sha512
  if [[ -f "${archive}" ]]; then
    actual_sha512="$(sha512sum "${archive}" | awk '{print $1}')"
  else
    actual_sha512=""
  fi
  if [[ "${actual_sha512}" != "${expected_sha512}" ]]; then
    staged="${tmp}/${name}.download"
    curl -fsSL "${url}" -o "${staged}"
    actual_sha512="$(sha512sum "${staged}" | awk '{print $1}')"
    if [[ "${actual_sha512}" != "${expected_sha512}" ]]; then
      echo "pinned source checksum mismatch: ${name}" >&2
      echo "expected ${expected_sha512}, got ${actual_sha512}" >&2
      exit 1
    fi
    mv -f "${staged}" "${archive}"
  fi
  printf '%s\n' "${archive}"
}

verify_pinned_apk() {
  local apk="$1" package="$2" version="$3" pkginfo
  pkginfo="$(tar --warning=no-unknown-keyword -xOzf "${apk}" .PKGINFO)"
  grep -Fxq "pkgname = ${package}" <<<"${pkginfo}" || {
    echo "pinned Alpine APK package mismatch: expected ${package}" >&2
    exit 1
  }
  grep -Fxq "pkgver = ${version}" <<<"${pkginfo}" || {
    echo "pinned Alpine APK version mismatch: expected ${package}-${version}" >&2
    exit 1
  }
}

runtime="${tmp}/runtime"
dev="${tmp}/dev"
mkdir -p "${runtime}" "${dev}"
for package in "${!wanted[@]}"; do
  case "${package}" in
    sway|wlroots)
      # Traverse their v3.22 dependency metadata, but replace both payloads
      # below. This keeps the proven v3.22 closure without shipping both
      # libwlroots-0.18.so and libwlroots-0.19.so.
      continue
      ;;
  esac
  tar --warning=no-unknown-keyword -xzf "$(download "${package}")" -C "${runtime}"
done

stock_sway_apk="$(download_pinned \
  "${stock_sway_branch}" community sway "${stock_sway_version}" "${stock_sway_sha256}")"
stock_wlroots_apk="$(download_pinned \
  "${stock_sway_branch}" community wlroots "${stock_wlroots_version}" "${stock_wlroots_sha256}")"
stock_display_info_apk="$(download_pinned \
  "${stock_sway_branch}" main libdisplay-info "${stock_display_info_version}" "${stock_display_info_sha256}")"
stock_liftoff_apk="$(download_pinned \
  "${stock_liftoff_branch}" community libliftoff "${stock_liftoff_version}" "${stock_liftoff_sha256}")"

verify_pinned_apk "${stock_sway_apk}" sway "${stock_sway_version}"
verify_pinned_apk "${stock_wlroots_apk}" wlroots "${stock_wlroots_version}"
verify_pinned_apk "${stock_display_info_apk}" libdisplay-info "${stock_display_info_version}"
verify_pinned_apk "${stock_liftoff_apk}" libliftoff "${stock_liftoff_version}"

# These four unmodified APKs are the complete branch-crossing overlay. All
# remaining runtime DSOs continue to come from the existing v3.22 roots.
for apk in \
  "${stock_display_info_apk}" \
  "${stock_liftoff_apk}" \
  "${stock_wlroots_apk}" \
  "${stock_sway_apk}"; do
  tar --warning=no-unknown-keyword -xzf "${apk}" -C "${runtime}"
done

if find "${runtime}" -name 'libwlroots-0.18.so*' -print -quit | grep -q .; then
  echo "stock Sway overlay unexpectedly contains wlroots 0.18" >&2
  exit 1
fi
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

# Alpine's binary font/icon packages do not carry their upstream license
# files. Keep the exact source notices next to the redistributed assets.
ipaex_source="$(download_source_pinned \
  IPAexfont00401.zip "${ipaex_source_url}" "${ipaex_source_sha512}")"
adwaita_source="$(download_source_pinned \
  adwaita-icon-theme-48.1.tar.xz \
  "${adwaita_source_url}" "${adwaita_source_sha512}")"
mkdir -p \
  "${runtime}/usr/share/licenses/font-ipaex" \
  "${runtime}/usr/share/licenses/adwaita-icon-theme"
python3 - "${ipaex_source}" \
  "${runtime}/usr/share/licenses/font-ipaex/IPA_Font_License_Agreement_v1.0.txt" <<'PY'
import sys
from pathlib import Path
from zipfile import ZipFile

archive = Path(sys.argv[1])
destination = Path(sys.argv[2])
member = "IPAexfont00401/IPA_Font_License_Agreement_v1.0.txt"
with ZipFile(archive) as source:
    destination.write_bytes(source.read(member))
PY
for notice in COPYING COPYING_CCBYSA3 COPYING_LGPL; do
  tar -xJOf "${adwaita_source}" \
    "adwaita-icon-theme-48.1/${notice}" \
    >"${runtime}/usr/share/licenses/adwaita-icon-theme/${notice}"
done

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

# APK triggers also populate the MIME database and gdk-pixbuf loader cache.
# gtk3-demo needs both even though PNG/JPEG support is built into libgdk_pixbuf.
host_update_mime_database=/usr/bin/update-mime-database
[[ -x "${host_update_mime_database}" ]] || {
  echo "missing host ${host_update_mime_database}" >&2
  exit 1
}
XDG_DATA_DIRS="${runtime}/usr/share" \
  "${host_update_mime_database}" "${runtime}/usr/share/mime" >/dev/null

pixbuf_dir="${runtime}/usr/lib/gdk-pixbuf-2.0/2.10.0"
pixbuf_cache="${pixbuf_dir}/loaders.cache"
GDK_PIXBUF_MODULEDIR="${pixbuf_dir}/loaders" \
  "${linux_musl}" \
  --library-path "${runtime}/lib:${runtime}/usr/lib" \
  "${runtime}/usr/bin/gdk-pixbuf-query-loaders" |
  /usr/bin/sed "s#${runtime}##g" >"${pixbuf_cache}"
[[ -s "${pixbuf_cache}" ]] || {
  echo "target gdk-pixbuf loader cache is empty" >&2
  exit 1
}

# This image intentionally has no Xwayland server. Use Alt for the guest
# modifier and disable the unavailable optional backend.
sed -i 's/^set \$mod Mod4$/set $mod Alt/' "${runtime}/etc/sway/config"
grep -Fxq 'set $mod Alt' "${runtime}/etc/sway/config" || {
  echo "failed to select Alt as the Sway modifier" >&2
  exit 1
}
mkdir -p "${runtime}/etc/sway/config.d"
printf 'xwayland disable\n' >"${runtime}/etc/sway/config.d/pacha.conf"

# Keep a true monospace face as Foot's primary font. IPAexGothic remains an
# explicit Japanese fallback, but is no longer mistaken for the terminal's
# Latin primary face and therefore does not trigger Foot's correctness warning.
foot_ini="${runtime}/etc/xdg/foot/foot.ini"
sed -i \
  's/^# font=monospace:size=8$/font=Roboto Mono:size=8,IPAexGothic:size=8/' \
  "${foot_ini}"
grep -Fxq 'font=Roboto Mono:size=8,IPAexGothic:size=8' "${foot_ini}" || {
  echo "failed to configure Foot's monospace primary font" >&2
  exit 1
}

# The persisted fixture encodes symlinks as marker files, so materialize a
# temporary flat library directory before asking the target musl loader to
# resolve every DT_NEEDED entry and relocation in the stock Sway executable.
loader_library_root="${tmp}/loader-libraries"
python3 - "${loader_library_root}" \
  "${runtime}" "${input_root}" "${mesa_root}" "${clang_root}" <<'PY'
import os
import sys
from pathlib import Path

destination = Path(sys.argv[1]).resolve()
roots = [Path(argument).resolve() for argument in sys.argv[2:]]
marker = b"CAPABILITYOS_ROOTFS_SYMLINK\n"
destination.mkdir(parents=True)


def resolve_payload(path, owner, seen):
    key = path.absolute()
    if key in seen:
        raise SystemExit(f"rootfs symlink loop while resolving {path}")
    seen.add(key)

    if path.is_symlink():
        target = os.readlink(path)
    elif path.is_file():
        with path.open("rb") as source:
            payload = source.read(4096)
        if not payload.startswith(marker):
            return path
        target = payload[len(marker):].decode()
    else:
        return None

    if target.startswith("/"):
        candidate = owner / target.lstrip("/")
    else:
        candidate = path.parent / target
    candidate = Path(os.path.normpath(candidate))
    if candidate.exists() or candidate.is_symlink():
        return resolve_payload(candidate, owner, seen)

    try:
        relative = candidate.relative_to(owner)
    except ValueError:
        relative = Path(target.lstrip("/"))
    for root in roots:
        alternate = root / relative
        if alternate.exists() or alternate.is_symlink():
            return resolve_payload(alternate, root, seen)
    return None


for root in roots:
    for relative in (Path("lib"), Path("usr/lib")):
        library_directory = root / relative
        if not library_directory.is_dir():
            continue
        for source in sorted(library_directory.iterdir()):
            target = destination / source.name
            if target.exists() or target.is_symlink():
                continue
            payload = resolve_payload(source, root, set())
            if payload is not None and payload.is_file():
                target.symlink_to(payload)
PY

loader_report="${tmp}/sway-loader.list"
if ! "${linux_musl}" \
  --library-path "${loader_library_root}" \
  --list "${runtime}/usr/bin/sway" >"${loader_report}" 2>&1; then
  cat "${loader_report}" >&2
  echo "stock Sway 1.11 does not resolve against the v3.22 fixture libraries" >&2
  exit 1
fi
for dependency in libwlroots-0.19.so libdisplay-info.so.3 libliftoff.so.0; do
  grep -Fq "${dependency} => ${loader_library_root}/${dependency}" "${loader_report}" || {
    cat "${loader_report}" >&2
    echo "stock Sway loader did not select pinned ${dependency}" >&2
    exit 1
  }
done
if grep -Fq 'libwlroots-0.18.so' "${loader_report}"; then
  cat "${loader_report}" >&2
  echo "stock Sway loader unexpectedly selected wlroots 0.18" >&2
  exit 1
fi

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
  usr/bin/gtk3-demo \
  etc/xdg/foot/foot.ini \
  usr/share/fonts/ipaexfont/ipaexg.ttf \
  'usr/share/fonts/roboto-mono/RobotoMono[wght].ttf' \
  usr/share/icons/Adwaita/index.theme \
  usr/share/licenses/font-ipaex/IPA_Font_License_Agreement_v1.0.txt \
  usr/share/licenses/adwaita-icon-theme/COPYING \
  usr/lib/libwlroots-0.19.so \
  usr/lib/libdisplay-info.so.3 \
  usr/lib/libliftoff.so.0 \
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
[[ -s "${runtime}/usr/share/mime/mime.cache" ]] || {
  echo "missing Sway runtime MIME cache" >&2
  exit 1
}
[[ -s "${runtime}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" ]] || {
  echo "missing Sway runtime gdk-pixbuf loader cache" >&2
  exit 1
}
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
printf 'v3.22 base closure (sway/wlroots payloads replaced):'
printf ' %s' "${!wanted[@]}"
printf '\nstock overlay packages: sway-%s wlroots-%s libdisplay-info-%s libliftoff-%s\n' \
  "${stock_sway_version}" "${stock_wlroots_version}" \
  "${stock_display_info_version}" "${stock_liftoff_version}"
printf 'input-overlay packages: seatd seatd-launch libseat libinput libinput-libs libinput-udev libevdev mtdev libudev-zero\n'
du -sh "${out_abs}" "${dev_out_abs}"
