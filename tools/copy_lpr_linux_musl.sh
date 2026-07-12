#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-linux-musl-libc.so}"

if [[ -n "${PACHAOS_LINUX_MUSL_LIBC:-}" ]]; then
  src="${PACHAOS_LINUX_MUSL_LIBC}"
else
  mirror="${PACHAOS_ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
  branch="${PACHAOS_ALPINE_BRANCH:-v3.20}"
  arch="${PACHAOS_ALPINE_ARCH:-x86_64}"
  cache="${repo_root}/.artifacts/third_party/alpine-lpr-musl"
  apkindex="${cache}/APKINDEX.tar.gz"
  index_txt="${cache}/APKINDEX"

  mkdir -p "${cache}"
  curl -fsSL "${mirror}/${branch}/main/${arch}/APKINDEX.tar.gz" -o "${apkindex}"
  tar -xOf "${apkindex}" APKINDEX > "${index_txt}"

  version="$(
    awk '
      BEGIN { RS=""; FS="\n" }
      {
        found = 0
        for (i = 1; i <= NF; i++) {
          if ($i == "P:musl") {
            found = 1
          }
        }
        if (found) {
          for (i = 1; i <= NF; i++) {
            if ($i ~ /^V:/) {
              print substr($i, 3)
              exit
            }
          }
        }
      }
    ' "${index_txt}"
  )"
  if [[ -z "${version}" ]]; then
    echo "musl package not found in ${mirror}/${branch}/main/${arch}" >&2
    exit 1
  fi

  apk="${cache}/musl-${version}.apk"
  root="${cache}/root"
  curl -fsSL "${mirror}/${branch}/main/${arch}/musl-${version}.apk" -o "${apk}"
  rm -rf "${root}"
  mkdir -p "${root}"
  tar -xzf "${apk}" -C "${root}" lib/ld-musl-x86_64.so.1
  src="${root}/lib/ld-musl-x86_64.so.1"
fi

mkdir -p "$(dirname "${repo_root}/${out}")"
cp "${src}" "${repo_root}/${out}"
