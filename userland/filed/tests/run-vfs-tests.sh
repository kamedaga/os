#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="${repo_root}/.artifacts/filed-tests"
cc_bin="${CC:-clang}"

mkdir -p "${build_dir}"

"${cc_bin}" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I"${repo_root}/userland/filed/include" \
  "${repo_root}/userland/filed/src/vfs.c" \
  "${repo_root}/userland/filed/tests/vfs_test.c" \
  -o "${build_dir}/vfs_test"

"${build_dir}/vfs_test"
