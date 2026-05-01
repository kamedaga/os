#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/bootstrap_vfs \
  -B .artifacts/cmake/bootstrap_vfs \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/bootstrap_vfs --target bootstrap_vfs
