#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/perf-estimate-20261021/retainwait/host-tests
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Iuserland/libipc/include -Iuserland/libpacha/include \
  tests/libipc_retained_wait_unit.c userland/libipc/src/ipc.c \
  -o .artifacts/perf-estimate-20261021/retainwait/host-tests/unit
.artifacts/perf-estimate-20261021/retainwait/host-tests/unit
