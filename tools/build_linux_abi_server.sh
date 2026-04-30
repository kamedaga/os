#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/linux_abi_server \
  -B .artifacts/cmake/linux_abi_server \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/linux_abi_server --target linux_abi_server
