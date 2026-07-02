#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-lua-dynamic-root}"
repo="${ALPINE_REPO:-https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64}"
cache="${repo_root}/.artifacts/third_party/alpine-lua-cli"

out_abs="${repo_root}/${out}"
mkdir -p "${cache}"
rm -rf "${out_abs}"
mkdir -p "${out_abs}/cmd" "${out_abs}/usr/lib"

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

fetch_apk() {
  local pkg="$1"
  local version
  version="$(apk_version "${pkg}")"
  if [[ -z "${version}" ]]; then
    echo "failed to resolve Alpine package ${pkg} from ${repo}" >&2
    exit 1
  fi
  local apk="${cache}/${pkg}-${version}.apk"
  curl -fsSL "${repo}/${pkg}-${version}.apk" -o "${apk}"
  printf '%s\n' "${apk}"
}

lua_apk="$(fetch_apk lua5.4)"
lua_libs_apk="$(fetch_apk lua5.4-libs)"
readline_apk="$(fetch_apk readline)"
ncurses_apk="$(fetch_apk libncursesw)"

tmp="$(mktemp -d "${cache}/extract-lua.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT

tar --warning=no-unknown-keyword -xzf "${lua_apk}" -C "${tmp}" usr/bin/lua5.4
tar --warning=no-unknown-keyword -xzf "${lua_libs_apk}" -C "${tmp}" usr/lib/lua5.4/liblua-5.4.so.0.0.0
tar --warning=no-unknown-keyword -xzf "${readline_apk}" -C "${tmp}" usr/lib/libreadline.so.8.3
tar --warning=no-unknown-keyword -xzf "${ncurses_apk}" -C "${tmp}" usr/lib/libncursesw.so.6.6

cp "${tmp}/usr/bin/lua5.4" "${out_abs}/cmd/alpine-lua5.4.elf"
cp "${tmp}/usr/lib/lua5.4/liblua-5.4.so.0.0.0" "${out_abs}/usr/lib/liblua-5.4.so.0"
cp "${tmp}/usr/lib/libreadline.so.8.3" "${out_abs}/usr/lib/libreadline.so.8"
cp "${tmp}/usr/lib/libncursesw.so.6.6" "${out_abs}/usr/lib/libncursesw.so.6"
chmod 0755 \
  "${out_abs}/cmd/alpine-lua5.4.elf" \
  "${out_abs}/usr/lib/liblua-5.4.so.0" \
  "${out_abs}/usr/lib/libreadline.so.8" \
  "${out_abs}/usr/lib/libncursesw.so.6"

if command -v readelf >/dev/null 2>&1; then
  readelf -l "${out_abs}/cmd/alpine-lua5.4.elf" >"${tmp}/lua.program-headers.txt"
  readelf -d "${out_abs}/cmd/alpine-lua5.4.elf" >"${tmp}/lua.dynamic.txt"
  grep -q '/lib/ld-musl-x86_64.so.1' "${tmp}/lua.program-headers.txt"
  grep -q 'liblua-5.4.so.0' "${tmp}/lua.dynamic.txt"
  grep -q 'libreadline.so.8' "${tmp}/lua.dynamic.txt"
  readelf -d "${out_abs}/usr/lib/libreadline.so.8" >"${tmp}/readline.dynamic.txt"
  grep -q 'libncursesw.so.6' "${tmp}/readline.dynamic.txt"
fi

printf 'downloaded Alpine lua5.4 into %s\n' "${out_abs}"
