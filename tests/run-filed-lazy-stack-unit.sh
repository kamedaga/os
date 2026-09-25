#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/filed-tests
"${CC:-/usr/bin/clang}" -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Iuserland/filed/include -Iuserland/filed/src -Iuserland/koboxd/include \
  -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/personality/include -I_kobox/include -I_kobox/src \
  tests/filed_lazy_stack_unit.c userland/filed/src/exec/linux_lpr/start.c \
  -o .artifacts/filed-tests/lazy-stack-unit
.artifacts/filed-tests/lazy-stack-unit
