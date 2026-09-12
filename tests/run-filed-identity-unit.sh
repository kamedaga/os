#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/filed-tests
clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Iuserland/filed/include -Iuserland/filed/src -Iuserland/koboxd/include \
  -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/personality/include -Iuserland/termd/include -I_kobox/include -I_kobox/src \
  tests/filed_identity_unit.c -o .artifacts/filed-tests/identity-unit
.artifacts/filed-tests/identity-unit
