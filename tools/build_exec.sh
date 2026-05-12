#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/exec \
  -B .artifacts/cmake/exec \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/exec --target exec
