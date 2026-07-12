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
input_root="${repo_root}/.artifacts/userland-fixtures/alpine-input-root"
out_abs="${repo_root}/${out}"
dev_out_abs="${repo_root}/${dev_out}"

[[ -d "${clang_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
[[ -d "${mesa_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_mesa.sh"
[[ -d "${input_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_input.sh"
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

for package in sway wlroots wayland wayland-protocols libxkbcommon pixman; do
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
for package in wayland-dev wayland-protocols; do
  tar --warning=no-unknown-keyword -xzf "$(download "${package}")" -C "${dev}"
done

rm -rf \
  "${runtime}"/.SIGN.* "${runtime}"/.PKGINFO "${runtime}"/.pre-* "${runtime}"/.post-* \
  "${runtime}"/etc/init.d "${runtime}"/etc/conf.d "${runtime}"/var \
  "${dev}"/.SIGN.* "${dev}"/.PKGINFO "${dev}"/.pre-* "${dev}"/.post-* \
  "${dev}"/etc "${dev}"/var

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
