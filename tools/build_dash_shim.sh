#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/dash_shim \
  -B .artifacts/cmake/dash_shim \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/dash_shim --target dash_shim
