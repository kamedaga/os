#!/usr/bin/env bash
set -euo pipefail

cmake \
  -S userland/esp_server \
  -B .artifacts/cmake/esp_server \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang

cmake --build .artifacts/cmake/esp_server --target esp_server esp_server_smoke
