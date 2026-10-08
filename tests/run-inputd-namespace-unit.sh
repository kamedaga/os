#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="${repo_root}/.artifacts/tests"
mkdir -p "$out_dir"
/usr/bin/clang -std=c11 -Wall -Wextra -Werror -Wno-sign-compare \
  -fsanitize=address,undefined \
  -I"${repo_root}/userland/inputd/include" \
  -I"${repo_root}/userland/filed/include" \
  -I"${repo_root}/userland/libfiled_client/include" \
  -I"${repo_root}/kobox2/protocol/include" \
  -I"${repo_root}/kobox2/protocol/generated/include" \
  -I"${repo_root}/userland/libipc/include" \
  -I"${repo_root}/userland/libpacha/include" \
  -I"${repo_root}/kobox2/linux-sandbox/kobox" \
  -I"${repo_root}/_kobox/src" \
  "${repo_root}/tests/inputd_namespace_unit.c" \
  "${repo_root}/kobox2/protocol/src/filesystem.c" \
  -o "${out_dir}/inputd-namespace-unit"
"${out_dir}/inputd-namespace-unit"
