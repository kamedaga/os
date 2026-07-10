#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/alpine-clang-root}"
alpine_version="${ALPINE_CLANG_VERSION:-v3.22}"
arch="${ALPINE_CLANG_ARCH:-x86_64}"
mirror="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
cache="${repo_root}/.artifacts/third_party/alpine-clang-${alpine_version}-${arch}"
out_abs="${repo_root}/${out}"

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
    busybox|busybox-binsh)
      return
      ;;
  esac
  if [[ -z "${queued[${pkg}]+x}" ]]; then
    queued["${pkg}"]=1
    queue+=("${pkg}")
  fi
}

for pkg in clang20 musl-dev binutils lld20; do
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

install_root="${tmp}/root"
mkdir -p "${install_root}"
for pkg in "${!wanted[@]}"; do
  version="$(package_field "${pkg}" V)"
  section="$(package_section "${pkg}")"
  apk="${cache}/${pkg}-${version}.apk"
  if [[ ! -f "${apk}" ]]; then
    curl -fsSL "${mirror}/${alpine_version}/${section}/${arch}/${pkg}-${version}.apk" -o "${apk}"
  fi
  tar --warning=no-unknown-keyword -xzf "${apk}" -C "${install_root}"
done

rm -rf \
  "${install_root}"/.SIGN.* \
  "${install_root}"/.PKGINFO \
  "${install_root}"/.INSTALL \
  "${install_root}"/etc \
  "${install_root}"/var \
  "${install_root}"/usr/share
python3 - "${install_root}" <<'PY'
import os
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
for link in sorted(path for path in root.rglob("*") if path.is_symlink()):
    target = os.readlink(link)
    resolved = (root / target.lstrip("/")) if target.startswith("/") else (link.parent / target)
    resolved = resolved.resolve(strict=False)
    if root not in resolved.parents and resolved != root:
        raise SystemExit(f"symlink escapes root: {link} -> {target}")
    if not resolved.exists():
        raise SystemExit(f"unresolved symlink: {link} -> {target}")
    link.unlink()
    relative = link.relative_to(root).as_posix()
    if relative in {"usr/bin/clang", "usr/bin/clang-20"}:
        link.write_bytes(resolved.read_bytes())
        link.chmod(resolved.stat().st_mode)
    else:
        link.write_bytes(b"CAPABILITYOS_ROOTFS_SYMLINK\n" + target.encode())
PY

rm -f \
  "${install_root}"/lib/ld-musl-x86_64.so.1 \
  "${install_root}"/lib/libc.musl-x86_64.so.1

for required in \
  usr/bin/clang \
  usr/bin/clang-20 \
  usr/lib/llvm20/bin/clang-20 \
  usr/bin/ld \
  usr/bin/ld.lld \
  usr/include/stdio.h \
  usr/lib/crt1.o \
  usr/lib/crti.o \
  usr/lib/crtn.o; do
  if [[ ! -e "${install_root}/${required}" ]]; then
    echo "missing clang rootfs dependency: /${required}" >&2
    exit 1
  fi
done

for pattern in \
  'usr/lib/libLLVM.so.20*' \
  'usr/lib/libclang-cpp.so.20*' \
  'usr/lib/libgcc_s.so.1' \
  'usr/lib/libstdc++.so.6'; do
  if ! compgen -G "${install_root}/${pattern}" >/dev/null; then
    echo "missing clang rootfs library: /${pattern}" >&2
    exit 1
  fi
done

readelf -l "${install_root}/usr/lib/llvm20/bin/clang-20" >"${tmp}/clang.program-headers"
readelf -d "${install_root}/usr/lib/llvm20/bin/clang-20" >"${tmp}/clang.dynamic"
grep -q '/lib/ld-musl-x86_64.so.1' "${tmp}/clang.program-headers"
grep -q 'libclang-cpp.so.20' "${tmp}/clang.dynamic"

rm -rf "${out_abs}.tmp"
mkdir -p "$(dirname "${out_abs}")"
mv "${install_root}" "${out_abs}.tmp"
rm -rf "${out_abs}"
mv "${out_abs}.tmp" "${out_abs}"

printf 'built Alpine clang root into %s\n' "${out_abs}"
printf 'packages:'
for pkg in "${!wanted[@]}"; do
  printf ' %s' "${pkg}"
done
printf '\n'
du -sh "${out_abs}"
