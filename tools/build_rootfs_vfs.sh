#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/rootfs/vfs \
  -B .artifacts/cmake/rootfs_vfs \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/rootfs_vfs --target rootfs_vfs
