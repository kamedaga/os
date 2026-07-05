#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:-.artifacts/third_party/mbedtls}"
out_abs="${repo_root}/${out}"
repo="${MBEDTLS_REPO:-https://github.com/Mbed-TLS/mbedtls.git}"
ref="${MBEDTLS_REF:-v2.28.10}"

mkdir -p "$(dirname "${out_abs}")"
if [[ ! -d "${out_abs}/.git" ]]; then
  rm -rf "${out_abs}"
  git clone --filter=blob:none "${repo}" "${out_abs}"
fi

git -C "${out_abs}" fetch --depth=1 origin "${ref}"
git -C "${out_abs}" checkout --detach FETCH_HEAD
git -C "${out_abs}" reset --hard FETCH_HEAD

cat >"${out_abs}/CAPABILITYOS_DEPENDENCY.txt" <<EOF
Mbed TLS source dependency
repo: ${repo}
ref: ${ref}

Mbed TLS is available under Apache-2.0 OR GPL-2.0-or-later. See the
upstream LICENSE and apache-2.0.txt files in this source tree.
EOF

printf 'downloaded Mbed TLS ref=%s into %s\n' "${ref}" "${out_abs}"
