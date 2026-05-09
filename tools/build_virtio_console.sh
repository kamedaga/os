#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/console/virtio_console \
  -B .artifacts/cmake/virtio_console \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/virtio_console --target virtio_console
