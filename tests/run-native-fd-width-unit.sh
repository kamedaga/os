#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/native-fd-width
mkdir -p "$out"
wrap=("-Wl,--wrap=pacha_ipc_registers")
for i in {0..6}; do wrap+=("-Wl,--wrap=pacha_syscall$i"); done
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections -Wl,--gc-sections "${wrap[@]}" \
  -Iuserland/libpacha/include -Iuserland/libipc/include -Iuserland/libcapsule/include \
  tests/native_fd_width_unit.c userland/libpacha/src/syscall.c \
  userland/libipc/src/ipc.c userland/libcapsule/src/capsule.c -o "$out/unit"
"$out/unit"
