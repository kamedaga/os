#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/.artifacts/tests/lpr-linux-memfd-seal-unit"
mkdir -p "${build_dir}"

/usr/bin/clang -std=c11 -Wall -Wextra -Werror \
  -I"${repo_root}/userland/personality/linux/runtime" \
  -I"${repo_root}/userland/libipc/include" \
  "${repo_root}/tests/lpr_linux_memfd_seal_unit.c" \
  "${repo_root}/userland/personality/linux/runtime/lpr_fd/memfd.c" \
  -o "${build_dir}/lpr-linux-memfd-seal-unit"

"${build_dir}/lpr-linux-memfd-seal-unit"
