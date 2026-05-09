#!/usr/bin/env bash
set -euo pipefail

cmake -S userland/tty_service \
  -B .artifacts/cmake/tty_service \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_ASM_COMPILER=clang
cmake --build .artifacts/cmake/tty_service --target tty_service
