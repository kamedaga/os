#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/tests/service-wait-batch
clang -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -fsanitize=address,undefined \
    -Iuserland/libipc/include -Iuserland/libpacha/include \
    userland/libipc/src/ipc.c tests/service_wait_batch_unit.c \
    -o .artifacts/tests/service-wait-batch/unit
.artifacts/tests/service-wait-batch/unit
