#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-apk-offline-root}"
main_repo="${ALPINE_REPO:-https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64}"
community_repo="${ALPINE_COMMUNITY_REPO:-https://dl-cdn.alpinelinux.org/alpine/latest-stable/community/x86_64}"
cache="${repo_root}/.artifacts/third_party/alpine-apk-offline"

# The seed packages are staged in a host-side repository used only to build the
# pre-populated package database below.  Nothing under it ships: the guest
# resolves every package from the network.  Shipping a mirror of the upstream
# index while carrying only a handful of the packages it names made apk pick
# the local repository and then fail with "package mentioned in index not
# found" for everything else.
localrepo="${cache}/localrepo"

out_abs="${repo_root}/${out}"
mkdir -p "${cache}"
rm -rf "${out_abs}" "${localrepo}"
mkdir -p \
  "${out_abs}/bin" \
  "${out_abs}/cmd" \
  "${out_abs}/usr/bin" \
  "${out_abs}/usr/lib" \
  "${out_abs}/opt/apk-offline/bin" \
  "${out_abs}/opt/apk-offline/lib" \
  "${out_abs}/etc/apk/keys" \
  "${localrepo}/main/x86_64" \
  "${localrepo}/community/x86_64"

main_index="${cache}/APKINDEX-main.tar.gz"
community_index="${cache}/APKINDEX-community.tar.gz"
main_index_text="${cache}/APKINDEX-main"
community_index_text="${cache}/APKINDEX-community"
index_text="${cache}/APKINDEX-combined"
curl -fsSL "${main_repo}/APKINDEX.tar.gz" -o "${main_index}"
curl -fsSL "${community_repo}/APKINDEX.tar.gz" -o "${community_index}"
tar -xOzf "${main_index}" APKINDEX >"${main_index_text}"
tar -xOzf "${community_index}" APKINDEX >"${community_index_text}"
cat "${main_index_text}" "${community_index_text}" >"${index_text}"

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

package_repo() {
  local pkg="$1"
  if awk -v want="${pkg}" '$0 == "P:" want { found = 1; exit } END { exit !found }' "${main_index_text}"; then
    printf '%s\n' "${main_repo}"
    return 0
  fi
  if awk -v want="${pkg}" '$0 == "P:" want { found = 1; exit } END { exit !found }' "${community_index_text}"; then
    printf '%s\n' "${community_repo}"
    return 0
  fi
  return 1
}

package_repo_name() {
  local pkg="$1"
  if awk -v want="${pkg}" '$0 == "P:" want { found = 1; exit } END { exit !found }' "${main_index_text}"; then
    printf 'main\n'
  else
    printf 'community\n'
  fi
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

for pkg in apk-tools alpine-keys nano zstd grep sed tar xz wget fastfetch; do
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
  pkg_repo="$(package_repo "${pkg}")"
  pkg_repo_name="$(package_repo_name "${pkg}")"
  if [[ ! -e "${apk}" ]]; then
    curl -fsSL "${pkg_repo}/${pkg}-${version}.apk" -o "${apk}"
  fi
  cp "${apk}" "${localrepo}/${pkg_repo_name}/x86_64/"

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
  if [[ -d "${tmp}/root/etc/apk/keys" ]]; then
    cp -a "${tmp}/root/etc/apk/keys/." "${out_abs}/etc/apk/keys/"
  fi
done

cp "${main_index}" "${localrepo}/main/x86_64/APKINDEX.tar.gz"
cp "${community_index}" "${localrepo}/community/x86_64/APKINDEX.tar.gz"
printf '%s\n' "${main_repo%/x86_64}" "${community_repo%/x86_64}" \
  >"${out_abs}/etc/apk/repositories"
cat >"${out_abs}/etc/apk/world" <<'EOF'
EOF
cat >"${out_abs}/etc/apk/arch" <<'EOF'
x86_64
EOF
cat >"${out_abs}/etc/apk/config" <<'EOF'
sync no
logfile no
EOF

cp "${out_abs}/opt/apk-offline/bin/apk" "${out_abs}/bin/apk"
cp "${out_abs}/opt/apk-offline/bin/apk" "${out_abs}/usr/bin/apk"
for library in libapk.so.3.0.0 libssl.so.3 libcrypto.so.3; do
  cp -a "${out_abs}/opt/apk-offline/lib/${library}" "${out_abs}/usr/lib/${library}"
done

musl_version="$(apk_field musl V)"
musl_apk="${cache}/musl-${musl_version}.apk"
mkdir -p "${tmp}/musl-runtime"
tar --warning=no-unknown-keyword -xzf "${musl_apk}" -C "${tmp}/musl-runtime"
install_root="${tmp}/installed-root"
mkdir -p \
  "${install_root}/etc/apk/keys" \
  "${install_root}/lib/apk/db" \
  "${install_root}/var/lib/apk"
cp -a "${out_abs}/etc/apk/keys/." "${install_root}/etc/apk/keys/"
cp "${out_abs}/etc/apk/arch" "${install_root}/etc/apk/arch"
printf '%s\n' \
  "${localrepo}/main" \
  "${localrepo}/community" \
  >"${install_root}/etc/apk/repositories"
LD_LIBRARY_PATH="${out_abs}/opt/apk-offline/lib" \
  "${tmp}/musl-runtime/lib/ld-musl-x86_64.so.1" \
  "${out_abs}/opt/apk-offline/bin/apk" \
  --root "${install_root}" \
  --usermode \
  --no-network \
  --no-cache \
  add --initdb
LD_LIBRARY_PATH="${out_abs}/opt/apk-offline/lib" \
  "${tmp}/musl-runtime/lib/ld-musl-x86_64.so.1" \
  "${out_abs}/opt/apk-offline/bin/apk" \
  --root "${install_root}" \
  --usermode \
  --no-network \
  --no-cache \
  --sync=no \
  --logfile=no \
  --repository "${localrepo}/main" \
  --repository "${localrepo}/community" \
  add nano grep
mkdir -p "${out_abs}/lib/apk/db"
cp -a "${install_root}/lib/apk/db/." "${out_abs}/lib/apk/db/"
cp "${install_root}/etc/apk/world" "${out_abs}/etc/apk/world"
cp "${install_root}/usr/bin/nano" "${out_abs}/usr/bin/nano"
cp -a "${install_root}/usr/bin/rnano" "${out_abs}/usr/bin/rnano"
cp "${install_root}/bin/grep" "${out_abs}/bin/grep"
cp "${install_root}/bin/grep" "${out_abs}/usr/bin/grep"

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
