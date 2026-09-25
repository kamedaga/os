#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
lock="${repo_root}/tools/manifests/ubuntu-kobox-nvme-x86_64.lock"
cache="${repo_root}/.artifacts/third_party/ubuntu-kobox-nvme"
out="${repo_root}/.artifacts/ubuntu-kobox-nvme"

for tool in ar curl file sha256sum tar zstd; do
  command -v "${tool}" >/dev/null 2>&1 || {
    echo "missing Ubuntu NVMe extraction tool: ${tool}" >&2
    exit 1
  }
done

read -r package version url expected_sha256 extra < <(awk '$1 !~ /^#/ { print; exit }' "${lock}")
[[ "${package}" == linux-modules-6.8.0-117-generic &&
   "${version}" == 6.8.0-117.117 &&
   "${url}" == https://archive.ubuntu.com/ubuntu/pool/main/l/linux/* &&
   "${expected_sha256}" =~ ^[0-9a-f]{64}$ && -z "${extra:-}" ]] || {
  echo "invalid Ubuntu NVMe lock: ${lock}" >&2
  exit 1
}

mkdir -p "${cache}"
deb="${cache}/${url##*/}"
actual_sha256=""
if [[ -f "${deb}" ]]; then
  actual_sha256="$(sha256sum "${deb}" | awk '{print $1}')"
fi
if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
  curl --fail --location --proto '=https' --tlsv1.2 --retry 3 \
    "${url}" -o "${deb}.download"
  printf '%s  %s\n' "${expected_sha256}" "${deb}.download" | sha256sum -c - >/dev/null
  mv "${deb}.download" "${deb}"
fi
printf '%s  %s\n' "${expected_sha256}" "${deb}" | sha256sum -c - >/dev/null

work="$(mktemp -d "${cache}/extract.XXXXXX")"
trap 'rm -rf "${work}"' EXIT
data_member="$(ar t "${deb}" | awk '$0 ~ /^data\.tar(\.(zst|xz|gz))?$/ { print; exit }')"
[[ -n "${data_member}" ]] || {
  echo "Ubuntu package has no supported data archive: ${deb}" >&2
  exit 1
}
ar p "${deb}" "${data_member}" >"${work}/${data_member}"

case "${data_member}" in
  data.tar) archive=(cat "${work}/${data_member}") ;;
  data.tar.zst) archive=(zstd -q -dc "${work}/${data_member}") ;;
  data.tar.xz) archive=(xz -dc "${work}/${data_member}") ;;
  data.tar.gz) archive=(gzip -dc "${work}/${data_member}") ;;
esac

module_root="./lib/modules/6.8.0-117-generic/kernel"
members=(
  "${module_root}/drivers/nvme/common/nvme-auth.ko.zst"
  "${module_root}/drivers/nvme/host/nvme-core.ko.zst"
  "${module_root}/drivers/nvme/host/nvme.ko.zst"
)
mkdir -p "${work}/root"
"${archive[@]}" | tar -x -C "${work}/root" "${members[@]}"

mkdir -p "${work}/out"
zstd -q -dc "${work}/root/${members[0]#./}" >"${work}/out/nvme-auth.ko"
zstd -q -dc "${work}/root/${members[1]#./}" >"${work}/out/nvme-core.ko"
zstd -q -dc "${work}/root/${members[2]#./}" >"${work}/out/nvme.ko"
for module in "${work}/out"/*.ko; do
  file "${module}" | grep -Fq 'ELF 64-bit LSB relocatable, x86-64' || {
    echo "invalid Ubuntu kernel module: ${module}" >&2
    exit 1
  }
done

rm -rf "${out}.old"
if [[ -e "${out}" ]]; then
  mv "${out}" "${out}.old"
fi
mv "${work}/out" "${out}"
rm -rf "${out}.old"
printf 'extracted pinned Ubuntu %s %s NVMe modules into %s\n' \
  "${package}" "${version}" "${out}"
