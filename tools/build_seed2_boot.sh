#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/seed2_boot \
  -B .artifacts/cmake/seed2_boot \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/seed2_boot --target seed2_boot
