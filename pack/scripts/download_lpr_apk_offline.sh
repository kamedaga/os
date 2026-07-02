#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-apk-offline-root}"
repo="${ALPINE_REPO:-https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64}"
cache="${repo_root}/.artifacts/third_party/alpine-apk-offline"

out_abs="${repo_root}/${out}"
mkdir -p "${cache}"
rm -rf "${out_abs}"
mkdir -p \
  "${out_abs}/cmd" \
  "${out_abs}/opt/apk-offline/bin" \
  "${out_abs}/opt/apk-offline/lib" \
  "${out_abs}/etc/apk" \
  "${out_abs}/var/cache/apk/offline/x86_64"

index="${cache}/APKINDEX.tar.gz"
curl -fsSL "${repo}/APKINDEX.tar.gz" -o "${index}"
index_text="${cache}/APKINDEX"
tar -xOzf "${index}" APKINDEX >"${index_text}"

apk_field() {
  local pkg="$1"
  local field="$2"
  awk -v want="${pkg}" -v field="${field}" '
    BEGIN { in_pkg = 0 }
    $0 == "P:" want { in_pkg = 1; next }
    in_pkg && index($0, field ":") == 1 { print substr($0, length(field) + 2); exit }
    in_pkg && /^$/ { in_pkg = 0 }
  ' "${index_text}"
}

normalize_dep() {
  local dep="$1"
  dep="${dep%%[<>=~]*}"
  dep="${dep#!}"
  printf '%s\n' "${dep}"
}

resolve_provider() {
  local dep="$1"
  if [[ -n "$(apk_field "${dep}" V)" ]]; then
    printf '%s\n' "${dep}"
    return 0
  fi
  awk -v want="${dep}" '
    BEGIN { in_pkg = 0; pkg = "" }
    /^P:/ { in_pkg = 1; pkg = substr($0, 3); next }
    in_pkg && /^p:/ {
      split(substr($0, 3), provides, " ")
      for (i in provides) {
        item = provides[i]
        sub(/[<>=~].*/, "", item)
        if (item == want) {
          print pkg
          exit
        }
      }
    }
    in_pkg && /^$/ { in_pkg = 0; pkg = "" }
  ' "${index_text}"
}

declare -A wanted=()
declare -A queued=()
queue=()

enqueue() {
  local pkg="$1"
  if [[ -z "${pkg}" ]]; then
    return
  fi
  local resolved
  resolved="$(resolve_provider "${pkg}")"
  if [[ -z "${resolved}" ]]; then
    echo "failed to resolve Alpine dependency ${pkg}" >&2
    exit 1
  fi
  if [[ -z "${queued[${resolved}]+x}" ]]; then
    queued["${resolved}"]=1
    queue+=("${resolved}")
  fi
}

for pkg in apk-tools zstd grep sed tar xz; do
  enqueue "${pkg}"
done

for ((i = 0; i < ${#queue[@]}; ++i)); do
  pkg="${queue[$i]}"
  wanted["${pkg}"]=1
  deps="$(apk_field "${pkg}" D || true)"
  for dep in ${deps}; do
    dep="$(normalize_dep "${dep}")"
    case "${dep}" in
      ""|musl|libc.musl-*) ;;
      *) enqueue "${dep}" ;;
    esac
  done
done

tmp="$(mktemp -d "${cache}/extract-apk-offline.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT

copy_if_exists() {
  local src="$1"
  local dst="$2"
  if [[ -e "${src}" ]]; then
    mkdir -p "$(dirname "${dst}")"
    cp -a "${src}" "${dst}"
  fi
}

for pkg in "${!wanted[@]}"; do
  version="$(apk_field "${pkg}" V)"
  if [[ -z "${version}" ]]; then
    echo "failed to resolve Alpine package ${pkg}" >&2
    exit 1
  fi
  apk="${cache}/${pkg}-${version}.apk"
  if [[ ! -e "${apk}" ]]; then
    curl -fsSL "${repo}/${pkg}-${version}.apk" -o "${apk}"
  fi
  cp "${apk}" "${out_abs}/var/cache/apk/offline/x86_64/"

  rm -rf "${tmp}/root"
  mkdir -p "${tmp}/root"
  tar --warning=no-unknown-keyword -xzf "${apk}" -C "${tmp}/root"
  copy_if_exists "${tmp}/root/sbin/apk" "${out_abs}/cmd/apk-offline.elf"
  copy_if_exists "${tmp}/root/sbin/apk" "${out_abs}/opt/apk-offline/bin/apk"
  copy_if_exists "${tmp}/root/bin/grep" "${out_abs}/opt/apk-offline/bin/grep"
  copy_if_exists "${tmp}/root/bin/sed" "${out_abs}/opt/apk-offline/bin/sed"
  copy_if_exists "${tmp}/root/bin/tar" "${out_abs}/opt/apk-offline/bin/tar"
  copy_if_exists "${tmp}/root/usr/bin/zstd" "${out_abs}/opt/apk-offline/bin/zstd"
  copy_if_exists "${tmp}/root/usr/bin/xz" "${out_abs}/opt/apk-offline/bin/xz"
  if [[ -d "${tmp}/root/usr/lib" ]]; then
    cp -a "${tmp}/root/usr/lib/." "${out_abs}/opt/apk-offline/lib/"
  fi
  if [[ -d "${tmp}/root/lib/apk" ]]; then
    mkdir -p "${out_abs}/opt/apk-offline/lib/apk"
    cp -a "${tmp}/root/lib/apk/." "${out_abs}/opt/apk-offline/lib/apk/"
  fi
done

cp "${index}" "${out_abs}/var/cache/apk/offline/x86_64/APKINDEX.tar.gz"
cat >"${out_abs}/etc/apk/repositories" <<'EOF'
/var/cache/apk/offline
EOF
cat >"${out_abs}/etc/apk/world" <<'EOF'
EOF
cat >"${out_abs}/etc/apk/arch" <<'EOF'
x86_64
EOF

find "${out_abs}" -type f -perm /111 -exec chmod 0755 {} +

if command -v readelf >/dev/null 2>&1; then
  readelf -l "${out_abs}/cmd/apk-offline.elf" >"${tmp}/apk.program-headers.txt"
  readelf -d "${out_abs}/cmd/apk-offline.elf" >"${tmp}/apk.dynamic.txt"
  grep -q '/lib/ld-musl-x86_64.so.1' "${tmp}/apk.program-headers.txt"
fi

printf 'downloaded offline Alpine apk workload into %s\n' "${out_abs}"
printf 'packages:'
for pkg in "${!wanted[@]}"; do
  printf ' %s' "${pkg}"
done
printf '\n'
