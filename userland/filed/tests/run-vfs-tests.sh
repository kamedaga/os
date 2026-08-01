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
  -pthread \
  -I"${repo_root}/userland/filed/include" \
  "${repo_root}/userland/filed/src/vfs/core.c" \
  "${repo_root}/userland/filed/src/vfs/object.c" \
  "${repo_root}/userland/filed/tests/vfs_test.c" \
  -o "${build_dir}/vfs_test"

"${build_dir}/vfs_test"

"${cc_bin}" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -pthread \
  -ffunction-sections \
  -fdata-sections \
  -Wl,--gc-sections \
  -I"${repo_root}/userland/filed/include" \
  -I"${repo_root}/userland/filed/src" \
  -I"${repo_root}/userland/koboxd/include" \
  -I"${repo_root}/userland/termd/include" \
  -I"${repo_root}/userland/drmd/include" \
  -I"${repo_root}/userland/inputd/include" \
  -I"${repo_root}/userland/lpr_supervisor/include" \
  -I"${repo_root}/userland/libipc/include" \
  -I"${repo_root}/userland/libpacha/include" \
  -I"${repo_root}/userland/personality/include" \
  "${repo_root}/userland/filed/src/vfs/core.c" \
  "${repo_root}/userland/filed/src/vfs/object.c" \
  "${repo_root}/userland/filed/src/dispatch/ops_file.c" \
  "${repo_root}/userland/filed/tests/setattr_dispatch_test.c" \
  -o "${build_dir}/setattr_dispatch_test"

"${build_dir}/setattr_dispatch_test"

"${cc_bin}" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -pthread \
  -I"${repo_root}/userland/filed/include" \
  -I"${repo_root}/userland/koboxd/include" \
  -I"${repo_root}/userland/termd/include" \
  -I"${repo_root}/userland/libipc/include" \
  "${repo_root}/userland/filed/src/tmpfs/backend.c" \
  "${repo_root}/userland/filed/src/tmpfs/dir.c" \
  "${repo_root}/userland/filed/src/tmpfs/file.c" \
  "${repo_root}/userland/filed/src/tmpfs/meta.c" \
  "${repo_root}/userland/filed/src/tmpfs/node.c" \
  "${repo_root}/userland/filed/src/tmpfs/page.c" \
  "${repo_root}/userland/filed/tests/tmpfs_backend_test.c" \
  -o "${build_dir}/tmpfs_backend_test"

"${build_dir}/tmpfs_backend_test"

"${cc_bin}" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -pthread \
  -I"${repo_root}/userland/filed/include" \
  -I"${repo_root}/userland/filed/src" \
  -I"${repo_root}/userland/koboxd/include" \
  -I"${repo_root}/userland/termd/include" \
  -I"${repo_root}/userland/libipc/include" \
  -I"${repo_root}/userland/libpacha/include" \
  -I"${repo_root}/userland/personality/include" \
  "${repo_root}/userland/filed/src/backend.c" \
  "${repo_root}/userland/filed/src/cache/cache.c" \
  "${repo_root}/userland/filed/src/tmpfs/backend.c" \
  "${repo_root}/userland/filed/src/tmpfs/dir.c" \
  "${repo_root}/userland/filed/src/tmpfs/file.c" \
  "${repo_root}/userland/filed/src/tmpfs/meta.c" \
  "${repo_root}/userland/filed/src/tmpfs/node.c" \
  "${repo_root}/userland/filed/src/tmpfs/page.c" \
  "${repo_root}/userland/filed/tests/cache_consistency_test.c" \
  -o "${build_dir}/cache_consistency_test"

"${build_dir}/cache_consistency_test"
