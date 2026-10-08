#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="${repo_root}/.artifacts/tests"
mkdir -p "$out_dir"
/usr/bin/clang -std=c11 -ffunction-sections -fdata-sections \
  -I"${repo_root}/userland/inputd/include" \
  -I"${repo_root}/userland/netd/include" \
  -I"${repo_root}/userland/libipc/include" \
  -I"${repo_root}/userland/libpacha/include" \
  -I"${repo_root}/userland/libfiled_client/include" \
  -I"${repo_root}/kobox2/linux-sandbox/kobox" \
  -I"${repo_root}/_kobox/src" \
  "${repo_root}/tests/inputd_publication_unit.c" -Wl,--gc-sections \
  -o "${out_dir}/inputd-publication-unit"
"${out_dir}/inputd-publication-unit"
