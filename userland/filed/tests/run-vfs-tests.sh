#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="${repo_root}/.artifacts/filed-tests"
cc_bin="${CC:-clang}"

mkdir -p "${build_dir}"
bash "${repo_root}/pack/scripts/download_miniz_tinfl.sh" >/dev/null

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

bash "${repo_root}/tests/run-filed-vmo-fd-growth-unit.sh"
bash "${repo_root}/tests/run-filed-vnode-eviction-scan-unit.sh"
bash "${repo_root}/tests/run-filed-vnode-growth-unit.sh"
bash "${repo_root}/tests/run-filed-open-growth-unit.sh"
bash "${repo_root}/tests/run-kobox-fs-objects-unit.sh"
bash "${repo_root}/tests/run-kobox-fs-readlink-unit.sh"

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
  -I"${repo_root}/userland/gpud/include" \
  -I"${repo_root}/userland/inputd/include" \
  -I"${repo_root}/userland/lpr_supervisor/include" \
  -I"${repo_root}/userland/libipc/include" \
  -I"${repo_root}/userland/libpacha/include" \
  -I"${repo_root}/userland/personality/include" \
  -I"${repo_root}/_kobox/include" \
  -I"${repo_root}/_kobox/src" \
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
  -std=c11 -Wall -Wextra -Werror -pthread \
  -I"${repo_root}/userland/filed/include" \
  -I"${repo_root}/.artifacts/third_party/miniz-3.0.2/source" \
  -I"${repo_root}/userland/koboxd/include" \
  -I"${repo_root}/userland/termd/include" \
  -I"${repo_root}/userland/libipc/include" \
  "${repo_root}/userland/filed/src/tmpfs/backend.c" \
  "${repo_root}/userland/filed/src/tmpfs/dir.c" \
  "${repo_root}/userland/filed/src/tmpfs/file.c" \
  "${repo_root}/userland/filed/src/tmpfs/meta.c" \
  "${repo_root}/userland/filed/src/tmpfs/node.c" \
  "${repo_root}/userland/filed/src/tmpfs/page.c" \
  "${repo_root}/userland/filed/src/live_bootfs.c" \
  "${repo_root}/.artifacts/third_party/miniz-3.0.2/source/miniz_tinfl.c" \
  "${repo_root}/userland/filed/tests/live_bootfs_test.c" \
  -lz \
  -o "${build_dir}/live_bootfs_test"

"${build_dir}/live_bootfs_test"

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
  "${repo_root}/userland/libipc/src/status.c" \
  "${repo_root}/userland/filed/src/tmpfs/backend.c" \
  "${repo_root}/userland/filed/src/tmpfs/dir.c" \
  "${repo_root}/userland/filed/src/tmpfs/file.c" \
  "${repo_root}/userland/filed/src/tmpfs/meta.c" \
  "${repo_root}/userland/filed/src/tmpfs/node.c" \
  "${repo_root}/userland/filed/src/tmpfs/page.c" \
  "${repo_root}/userland/filed/tests/cache_consistency_test.c" \
  -Wl,--wrap=calloc \
  -o "${build_dir}/cache_consistency_test"

"${build_dir}/cache_consistency_test"

"${cc_bin}" \
  -std=c11 \
  -D_POSIX_C_SOURCE=200809L \
  -Wall \
  -Wextra \
  -Werror \
  -I"${repo_root}/userland/filed/include" \
  -I"${repo_root}/userland/koboxd/include" \
  -I"${repo_root}/userland/libipc/include" \
  -I"${repo_root}/userland/libpacha/include" \
  "${repo_root}/userland/filed/src/kobox_backend.c" \
  "${repo_root}/userland/filed/tests/kobox_backend_link_test.c" \
  -o "${build_dir}/kobox_backend_link_test"

"${build_dir}/kobox_backend_link_test"

"${cc_bin}" \
  -std=c11 \
  -D_POSIX_C_SOURCE=200809L \
  -Wall \
  -Wextra \
  -Werror \
  -I"${repo_root}/userland/filed/include" \
  -I"${repo_root}/userland/filed/src" \
  -I"${repo_root}/userland/libipc/include" \
  "${repo_root}/userland/filed/src/exec/linux_lpr/script.c" \
  "${repo_root}/userland/filed/tests/shebang_test.c" \
  -o "${build_dir}/shebang_test"

"${build_dir}/shebang_test"
