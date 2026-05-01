#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/rootfs/fat_server \
  -B .artifacts/cmake/rootfs_fat_server \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/rootfs_fat_server --target fat_server
