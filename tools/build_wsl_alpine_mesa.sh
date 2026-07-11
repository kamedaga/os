#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/alpine-mesa-root}"
dev_out="${2:-.artifacts/userland-fixtures/alpine-mesa-dev-root}"
alpine_version="${ALPINE_MESA_VERSION:-v3.22}"
arch="${ALPINE_MESA_ARCH:-x86_64}"
mirror="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
cache="${repo_root}/.artifacts/third_party/alpine-mesa-${alpine_version}-${arch}"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
out_abs="${repo_root}/${out}"
dev_out_abs="${repo_root}/${dev_out}"

if [[ ! -d "${clang_root}" ]]; then
  bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
fi

mkdir -p "${cache}"
tmp="$(mktemp -d "${cache}/extract.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT

for section in main community; do
  index_archive="${cache}/${section}-APKINDEX.tar.gz"
  curl -fsSL "${mirror}/${alpine_version}/${section}/${arch}/APKINDEX.tar.gz" -o "${index_archive}"
  tar -xOzf "${index_archive}" APKINDEX >"${cache}/${section}-APKINDEX"
done

apk_field_in() {
  local index="$1"
  local pkg="$2"
  local field="$3"
  awk -v want="${pkg}" -v field="${field}" '
    BEGIN { in_pkg = 0 }
    $0 == "P:" want { in_pkg = 1; next }
    in_pkg && index($0, field ":") == 1 { print substr($0, length(field) + 2); exit }
    in_pkg && /^$/ { in_pkg = 0 }
  ' "${index}"
}

package_section() {
  local pkg="$1"
  for section in main community; do
    if [[ -n "$(apk_field_in "${cache}/${section}-APKINDEX" "${pkg}" V)" ]]; then
      printf '%s\n' "${section}"
      return 0
    fi
  done
  return 1
}

package_field() {
  local pkg="$1"
  local field="$2"
  local section
  section="$(package_section "${pkg}")" || return 1
  apk_field_in "${cache}/${section}-APKINDEX" "${pkg}" "${field}"
}

normalize_dep() {
  local dep="$1"
  dep="${dep#!}"
  dep="${dep%%[<>=~]*}"
  printf '%s\n' "${dep}"
}

resolve_provider() {
  local dep="$1"
  if package_section "${dep}" >/dev/null 2>&1; then
    printf '%s\n' "${dep}"
    return 0
  fi
  for section in main community; do
    local provider
    provider="$(awk -v want="${dep}" '
      BEGIN { pkg = "" }
      /^P:/ { pkg = substr($0, 3); next }
      /^p:/ {
        split(substr($0, 3), provides, " ")
        for (i in provides) {
          item = provides[i]
          sub(/[<>=~].*/, "", item)
          if (item == want) { print pkg; exit }
        }
      }
      /^$/ { pkg = "" }
    ' "${cache}/${section}-APKINDEX")"
    if [[ -n "${provider}" ]]; then
      printf '%s\n' "${provider}"
      return 0
    fi
  done
  return 1
}

declare -A queued=()
declare -A wanted=()
queue=()

enqueue() {
  local dep="$1"
  local pkg
  case "${dep}" in
    ""|libc.musl-*|so:libc.musl-*|busybox|busybox-binsh|cmd:sh)
      return
      ;;
  esac
  pkg="$(resolve_provider "${dep}")" || {
    echo "failed to resolve Alpine dependency ${dep}" >&2
    exit 1
  }
  case "${pkg}" in
    busybox|busybox-binsh|musl)
      return
      ;;
  esac
  if [[ -z "${queued[${pkg}]+x}" ]]; then
    queued["${pkg}"]=1
    queue+=("${pkg}")
  fi
}

for pkg in mesa-gl mesa-egl mesa-gbm mesa-gles mesa-dri-gallium; do
  enqueue "${pkg}"
done

for ((i = 0; i < ${#queue[@]}; ++i)); do
  pkg="${queue[$i]}"
  wanted["${pkg}"]=1
  deps="$(package_field "${pkg}" D || true)"
  for dep in ${deps}; do
    enqueue "$(normalize_dep "${dep}")"
  done
done

download_package() {
  local pkg="$1"
  local version section apk
  version="$(package_field "${pkg}" V)"
  section="$(package_section "${pkg}")"
  apk="${cache}/${pkg}-${version}.apk"
  if [[ ! -f "${apk}" ]]; then
    curl -fsSL "${mirror}/${alpine_version}/${section}/${arch}/${pkg}-${version}.apk" -o "${apk}"
  fi
  printf '%s\n' "${apk}"
}

install_root="${tmp}/root"
mkdir -p "${install_root}"
for pkg in "${!wanted[@]}"; do
  apk="$(download_package "${pkg}")"
  tar --warning=no-unknown-keyword -xzf "${apk}" -C "${install_root}"
done

dev_root="${tmp}/dev-root"
mkdir -p "${dev_root}"
for pkg in mesa-dev libdrm-dev linux-headers; do
  apk="$(download_package "${pkg}")"
  tar --warning=no-unknown-keyword -xzf "${apk}" -C "${dev_root}"
done

rm -rf \
  "${install_root}"/.SIGN.* \
  "${install_root}"/.PKGINFO \
  "${install_root}"/.INSTALL \
  "${install_root}"/etc \
  "${install_root}"/var \
  "${dev_root}"/.SIGN.* \
  "${dev_root}"/.PKGINFO \
  "${dev_root}"/.INSTALL \
  "${dev_root}"/etc \
  "${dev_root}"/var \
  "${dev_root}"/usr/share

python3 - "${install_root}" "${dev_root}" "${clang_root}" <<'PY'
import os
import sys
from pathlib import Path

runtime = Path(sys.argv[1]).resolve()
dev = Path(sys.argv[2]).resolve()
clang = Path(sys.argv[3]).resolve()

def materialize_links(root: Path) -> None:
    for link in sorted(path for path in root.rglob("*") if path.is_symlink()):
        target = os.readlink(link)
        resolved = (root / target.lstrip("/")) if target.startswith("/") else (link.parent / target)
        resolved = resolved.resolve(strict=False)
        if root not in resolved.parents and resolved != root:
            raise SystemExit(f"symlink escapes root: {link} -> {target}")
        link.unlink()
        link.write_bytes(b"CAPABILITYOS_ROOTFS_SYMLINK\n" + target.encode())

materialize_links(runtime)

# clang and Mesa use the same Alpine branch. Keep the Mesa overlay incremental and
# fail loudly if two packages would install different bytes at the same rootfs path.
for path in sorted(p for p in runtime.rglob("*") if p.is_file()):
    relative = path.relative_to(runtime)
    shared = clang / relative
    if not shared.is_file():
        continue
    if path.read_bytes() != shared.read_bytes():
        raise SystemExit(f"Mesa/clang rootfs collision differs: /{relative}")
    path.unlink()

for directory in sorted((p for p in runtime.rglob("*") if p.is_dir()), reverse=True):
    try:
        directory.rmdir()
    except OSError:
        pass
PY

for pattern in \
  'usr/lib/libEGL.so.1' \
  'usr/lib/libgbm.so.1' \
  'usr/lib/libGLESv2.so.2' \
  'usr/lib/libGL.so.1' \
  'usr/lib/libgallium-*.so' \
  'usr/lib/dri/kms_swrast_dri.so'; do
  if ! compgen -G "${install_root}/${pattern}" >/dev/null; then
    echo "missing Mesa rootfs dependency: /${pattern}" >&2
    exit 1
  fi
done

for required in usr/include/EGL/egl.h usr/include/GLES2/gl2.h usr/include/gbm.h usr/include/xf86drmMode.h; do
  if [[ ! -e "${dev_root}/${required}" ]]; then
    echo "missing Mesa fixture header: /${required}" >&2
    exit 1
  fi
done

rm -rf "${out_abs}.tmp" "${dev_out_abs}.tmp"
mkdir -p "$(dirname "${out_abs}")" "$(dirname "${dev_out_abs}")"
mv "${install_root}" "${out_abs}.tmp"
mv "${dev_root}" "${dev_out_abs}.tmp"
rm -rf "${out_abs}" "${dev_out_abs}"
mv "${out_abs}.tmp" "${out_abs}"
mv "${dev_out_abs}.tmp" "${dev_out_abs}"

printf 'built Alpine Mesa runtime root into %s\n' "${out_abs}"
printf 'runtime packages:'
for pkg in "${!wanted[@]}"; do
  printf ' %s' "${pkg}"
done
printf '\n'
du -sh "${out_abs}" "${dev_out_abs}"
