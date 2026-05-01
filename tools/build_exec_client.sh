#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/exec_client \
  -B .artifacts/cmake/exec_client \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/exec_client --target exec_client
