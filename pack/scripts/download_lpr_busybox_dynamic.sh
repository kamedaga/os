#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-busybox-dynamic-root}"
repo="${ALPINE_REPO:-https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64}"
cache="${repo_root}/.artifacts/third_party/alpine-lpr-cli"

out_abs="${repo_root}/${out}"
mkdir -p "${cache}"
rm -rf "${out_abs}"
mkdir -p "${out_abs}/cmd"

index="${cache}/APKINDEX.tar.gz"
curl -fsSL "${repo}/APKINDEX.tar.gz" -o "${index}"
index_text="${cache}/APKINDEX"
tar -xOzf "${index}" APKINDEX >"${index_text}"

apk_version() {
  local pkg="$1"
  awk -v want="${pkg}" '
    BEGIN { in_pkg = 0 }
    $0 == "P:" want { in_pkg = 1; next }
    in_pkg && /^V:/ { print substr($0, 3); exit }
    in_pkg && /^$/ { in_pkg = 0 }
  ' "${index_text}"
}

busybox_version="$(apk_version busybox)"
if [[ -z "${busybox_version}" ]]; then
  echo "failed to resolve Alpine busybox version from ${repo}" >&2
  exit 1
fi

busybox_apk="${cache}/busybox-${busybox_version}.apk"
curl -fsSL "${repo}/busybox-${busybox_version}.apk" -o "${busybox_apk}"

tmp="$(mktemp -d "${cache}/extract.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT

tar --warning=no-unknown-keyword -xzf "${busybox_apk}" -C "${tmp}" bin/busybox

cp "${tmp}/bin/busybox" "${out_abs}/cmd/alpine-busybox.elf"
chmod 0755 "${out_abs}/cmd/alpine-busybox.elf"

if command -v readelf >/dev/null 2>&1; then
  readelf -l "${out_abs}/cmd/alpine-busybox.elf" >"${tmp}/busybox.program-headers.txt"
  readelf -d "${out_abs}/cmd/alpine-busybox.elf" >"${tmp}/busybox.dynamic.txt"
  grep -q '/lib/ld-musl-x86_64.so.1' "${tmp}/busybox.program-headers.txt"
  grep -q 'libc.musl-x86_64.so.1' "${tmp}/busybox.dynamic.txt"
fi

printf 'downloaded Alpine busybox=%s into %s\n' \
  "${busybox_version}" "${out_abs}"
