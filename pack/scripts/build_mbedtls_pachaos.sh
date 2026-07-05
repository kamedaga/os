#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
src_rel="${MBEDTLS_SOURCE_DIR:-.artifacts/third_party/mbedtls}"
src="${repo_root}/${src_rel}"
out_rel="${MBEDTLS_PACHAOS_OUT:-.artifacts/mbedtls-pachaos}"
out="${repo_root}/${out_rel}"
build="${repo_root}/.artifacts/build/mbedtls-pachaos"
sysroot="${build}/sysroot"
obj_dir="${build}/obj"
cc="${CAPOS_FREESTANDING_CC:-clang}"
ar="${CAPOS_AR:-ar}"
target="${PACHAOS_MUSL_DRIVER_TARGET:-x86_64-linux-musl}"
musl="${repo_root}/musl/upstream"

bash "${repo_root}/pack/scripts/download_mbedtls.sh" "${src_rel}"

rm -rf "${build}" "${out}"
mkdir -p \
  "${obj_dir}" \
  "${sysroot}/usr/include" \
  "${sysroot}/usr/include/bits" \
  "${sysroot}/usr/include/pachaos" \
  "${out}/lib" \
  "${out}/include"

cp -R "${musl}/include/." "${sysroot}/usr/include/"
cp -R "${musl}/arch/generic/bits/." "${sysroot}/usr/include/bits/"
cp -R "${musl}/arch/pachaos/bits/." "${sysroot}/usr/include/bits/"
sed -f "${musl}/tools/mkalltypes.sed" \
  "${musl}/arch/pachaos/bits/alltypes.h.in" \
  "${musl}/include/alltypes.h.in" \
  > "${sysroot}/usr/include/bits/alltypes.h"
cp "${musl}/arch/pachaos/bits/syscall.h.in" "${sysroot}/usr/include/bits/syscall.h"
sed -n -e 's/__NR_/SYS_/p' "${musl}/arch/pachaos/bits/syscall.h.in" >> "${sysroot}/usr/include/bits/syscall.h"
cp "${repo_root}/musl/pachaos/include/pachaos/abi.h" "${sysroot}/usr/include/pachaos/abi.h"

common_cflags=(
  -target "${target}"
  --sysroot "${sysroot}"
  -std=c99
  -fno-stack-protector
  -fno-plt
  -fPIE
  -mno-red-zone
  -O2
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-unused-function
  -Wno-missing-field-initializers
  -D__pachaos__
  -D_XOPEN_SOURCE=700
  -DMBEDTLS_NO_UDBL_DIVISION
  -DMBEDTLS_CONFIG_FILE='<mbedtls/config.h>'
  -I "${src}/include"
)

objects=()
index=0
while IFS= read -r source; do
  base="$(basename "${source}" .c)"
  obj="${obj_dir}/${index}_${base}.o"
  "${cc}" "${common_cflags[@]}" -c "${source}" -o "${obj}"
  objects+=("${obj}")
  index=$((index + 1))
done < <(find "${src}/library" -maxdepth 1 -name '*.c' | sort)

"${ar}" rcs "${out}/lib/libmbedtls-pachaos.a" "${objects[@]}"
cp -R "${src}/include/." "${out}/include/"

cat >"${out}/BUILD_INFO.txt" <<EOF
Mbed TLS PachaOS static archive
source: ${src_rel}
archive: ${out_rel}/lib/libmbedtls-pachaos.a
EOF

printf '%s\n' "${out}/lib/libmbedtls-pachaos.a"
