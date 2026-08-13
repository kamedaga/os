#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="${repo_root}/.artifacts/tests"
mkdir -p "$out_dir"

/usr/bin/clang \
  -std=c11 \
  -D_POSIX_C_SOURCE=200809L \
  -ffunction-sections \
  -fdata-sections \
  -I"${repo_root}/userland/inputd/include" \
  -I"${repo_root}/userland/inputd/src" \
  -I"${repo_root}/userland/libipc/include" \
  -I"${repo_root}/userland/libcapsule/include" \
  -I"${repo_root}/userland/libpacha/include" \
  -I"${repo_root}/userland/libfiled_client/include" \
  -I"${repo_root}/_kobox/include" \
  -I"${repo_root}/_kobox/src" \
  "${repo_root}/tests/inputd_ring_overflow_unit.c" \
  -Wl,--gc-sections \
  -o "${out_dir}/inputd-ring-overflow-unit"

"${out_dir}/inputd-ring-overflow-unit"
