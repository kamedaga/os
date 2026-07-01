#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-busybox-dynamic-root}"
repo="${ALPINE_REPO:-https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64}"
cache="${repo_root}/.artifacts/third_party/alpine-lpr-cli"

out_abs="${repo_root}/${out}"
mkdir -p "${cache}" "${out_abs}/cmd" "${out_abs}/lib"

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
musl_version="$(apk_version musl)"
if [[ -z "${busybox_version}" || -z "${musl_version}" ]]; then
  echo "failed to resolve Alpine busybox/musl versions from ${repo}" >&2
  exit 1
fi

busybox_apk="${cache}/busybox-${busybox_version}.apk"
musl_apk="${cache}/musl-${musl_version}.apk"
curl -fsSL "${repo}/busybox-${busybox_version}.apk" -o "${busybox_apk}"
curl -fsSL "${repo}/musl-${musl_version}.apk" -o "${musl_apk}"

tmp="$(mktemp -d "${cache}/extract.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT

tar --warning=no-unknown-keyword -xzf "${busybox_apk}" -C "${tmp}" bin/busybox
tar --warning=no-unknown-keyword -xzf "${musl_apk}" -C "${tmp}" lib/ld-musl-x86_64.so.1

cp "${tmp}/bin/busybox" "${out_abs}/cmd/alpine-busybox.elf"
cp "${tmp}/lib/ld-musl-x86_64.so.1" "${out_abs}/lib/libc.musl-x86_64.so.1"
chmod 0755 "${out_abs}/cmd/alpine-busybox.elf" "${out_abs}/lib/libc.musl-x86_64.so.1"

if command -v readelf >/dev/null 2>&1; then
  readelf -l "${out_abs}/cmd/alpine-busybox.elf" >"${tmp}/busybox.program-headers.txt"
  readelf -d "${out_abs}/cmd/alpine-busybox.elf" >"${tmp}/busybox.dynamic.txt"
  grep -q '/lib/ld-musl-x86_64.so.1' "${tmp}/busybox.program-headers.txt"
  grep -q 'libc.musl-x86_64.so.1' "${tmp}/busybox.dynamic.txt"
fi

printf 'downloaded Alpine busybox=%s musl=%s into %s\n' \
  "${busybox_version}" "${musl_version}" "${out_abs}"
