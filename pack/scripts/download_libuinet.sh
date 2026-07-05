#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:-.artifacts/third_party/libuinet}"
out_abs="${repo_root}/${out}"
repo="${LIBUINET_REPO:-https://github.com/pkelsey/libuinet.git}"
ref="${LIBUINET_REF:-253e24687886cec583331b56d594b60ed3cd0410}"

mkdir -p "$(dirname "${out_abs}")"
if [[ ! -d "${out_abs}/.git" ]]; then
  rm -rf "${out_abs}"
  git clone --filter=blob:none "${repo}" "${out_abs}"
fi

git -C "${out_abs}" fetch --depth=1 origin "${ref}"
git -C "${out_abs}" checkout --detach FETCH_HEAD
git -C "${out_abs}" reset --hard FETCH_HEAD

"${repo_root}/pack/scripts/apply_libuinet_pachaos_overlay.sh" "${out_abs}"

cat >"${out_abs}/CAPABILITYOS_DEPENDENCY.txt" <<EOF
libuinet source dependency
repo: ${repo}
ref: ${ref}

libuinet is a user-space port of the FreeBSD TCP/IP stack. It does not
provide a single top-level license grant; retain and review the copyright
and license notices in the individual upstream source files before
redistribution of built artifacts.
EOF

printf 'downloaded libuinet ref=%s into %s\n' "${ref}" "${out_abs}"
