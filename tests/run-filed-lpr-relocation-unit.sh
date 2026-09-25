#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/filed-tests
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/filed_lpr_relocation_unit.c -o .artifacts/filed-tests/lpr-relocation-unit
.artifacts/filed-tests/lpr-relocation-unit
