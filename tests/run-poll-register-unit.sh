#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/tests/poll-register
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iuserland/libpacha/include tests/poll_register_unit.c -o .artifacts/tests/poll-register/unit
.artifacts/tests/poll-register/unit
