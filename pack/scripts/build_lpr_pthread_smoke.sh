#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out_base="${1:-.artifacts/userland-fixtures/lpr_pthread_smoke}"
src="${2:-${repo_root}/userland/fixtures/src/wsl_musl/lpr_pthread_smoke.c}"
cc="${PACHAOS_HOST_CLANG:-/usr/bin/clang}"
mirror="${PACHAOS_ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
branch="${PACHAOS_ALPINE_BRANCH:-v3.18}"
arch="${PACHAOS_ALPINE_ARCH:-x86_64}"
cache="${repo_root}/.artifacts/third_party/alpine-lpr-pthread"
sysroot="${cache}/sysroot"
runtime="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"

mkdir -p "${cache}"
index="${cache}/APKINDEX"
if [[ ! -e "${index}" ]]; then
  curl -fsSL "${mirror}/${branch}/main/${arch}/APKINDEX.tar.gz" -o "${cache}/APKINDEX.tar.gz"
  tar -xOf "${cache}/APKINDEX.tar.gz" APKINDEX >"${index}"
fi
musl_dev_version="$(
  awk '
    BEGIN { RS=""; FS="\n" }
    {
      found = 0
      for (i = 1; i <= NF; i++) if ($i == "P:musl-dev") found = 1
      if (found) for (i = 1; i <= NF; i++) if ($i ~ /^V:/) {
        print substr($i, 3)
        exit
      }
    }
  ' "${index}"
)"
if [[ -z "${musl_dev_version}" ]]; then
  echo "failed to resolve Alpine musl-dev" >&2
  exit 1
fi
musl_dev_apk="${cache}/musl-dev-${musl_dev_version}.apk"
if [[ ! -e "${musl_dev_apk}" ]]; then
  curl -fsSL "${mirror}/${branch}/main/${arch}/musl-dev-${musl_dev_version}.apk" -o "${musl_dev_apk}"
fi
if [[ ! -e "${runtime}" ]]; then
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"
fi

rm -rf "${sysroot}"
mkdir -p "${sysroot}/lib"
tar --warning=no-unknown-keyword -xzf "${musl_dev_apk}" -C "${sysroot}"
cp "${runtime}" "${sysroot}/lib/ld-musl-x86_64.so.1"

out_abs="${repo_root}/${out_base}"
mkdir -p "$(dirname "${out_abs}")"
obj="${out_abs}.o"
"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -std=c11 \
  -O2 \
  -fPIC \
  -pthread \
  -c "${src}" \
  -o "${obj}"

"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -nostdlib \
  -static \
  "${sysroot}/usr/lib/crt1.o" \
  "${sysroot}/usr/lib/crti.o" \
  "${obj}" \
  -L"${sysroot}/usr/lib" \
  -lc \
  "${sysroot}/usr/lib/crtn.o" \
  -o "${out_abs}.static.elf"

"${cc}" \
  -target x86_64-linux-musl \
  --sysroot="${sysroot}" \
  -nostdlib \
  "${sysroot}/usr/lib/Scrt1.o" \
  "${sysroot}/usr/lib/crti.o" \
  "${obj}" \
  -L"${sysroot}/usr/lib" \
  -L"${sysroot}/lib" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  -lc \
  "${sysroot}/usr/lib/crtn.o" \
  -o "${out_abs}.dynamic.elf"

chmod 0755 "${out_abs}.static.elf" "${out_abs}.dynamic.elf"
rm -f "${obj}"

readelf -h "${out_abs}.static.elf" | grep -q 'Class:.*ELF64'
if readelf -l "${out_abs}.static.elf" | grep -q 'Requesting program interpreter'; then
  echo "static pthread smoke unexpectedly has an interpreter" >&2
  exit 1
fi
readelf -l "${out_abs}.dynamic.elf" | grep -q '/lib/ld-musl-x86_64.so.1'

printf 'built static and dynamic LPR musl fixtures at %s.{static,dynamic}.elf\n' "${out_abs}"
