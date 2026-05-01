#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/exec_service \
  -B .artifacts/cmake/exec_service \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/exec_service --target exec_service
