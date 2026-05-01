#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/seed2_root \
  -B .artifacts/cmake/seed2_root \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/seed2_root --target seed2_root
