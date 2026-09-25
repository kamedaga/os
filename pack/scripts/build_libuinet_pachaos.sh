#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
src_rel="${LIBUINET_SOURCE_DIR:-.artifacts/third_party/libuinet}"
src="${repo_root}/${src_rel}"
out_rel="${LIBUINET_PACHAOS_OUT:-.artifacts/libuinet-pachaos}"
out="${repo_root}/${out_rel}"

"${repo_root}/pack/scripts/download_libuinet.sh" "${src_rel}"

make -C "${src}" config
make -C "${src}/lib/libuinet" clean
# This pinned FreeBSD-derived tree treats every host-compiler warning as an
# error.  New GCC predefined macros and diagnostics are outside its source
# contract, so keep the upstream sources untouched and do not promote them.
make -C "${src}/lib/libuinet" PACHAOS_ONLY=1 MK_SSP=no WERROR= libuinet.a

rm -rf "${out}"
mkdir -p "${out}/lib" "${out}/include"
cp "${src}/lib/libuinet/libuinet.a" "${out}/lib/libuinet.a"
cp -R "${src}/lib/libuinet/api_include/." "${out}/include/"

cat >"${out}/BUILD_INFO.txt" <<EOF
libuinet PachaOS static archive
source: ${src_rel}
archive: ${out_rel}/lib/libuinet.a
EOF

printf '%s\n' "${out}/lib/libuinet.a"
