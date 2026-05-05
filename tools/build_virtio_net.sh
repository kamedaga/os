#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/net/virtio_net \
  -B .artifacts/cmake/virtio_net \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/virtio_net --target virtio_net
