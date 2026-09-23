#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/filed-tmpfs-page-alloc
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iuserland/filed/include -Iuserland/koboxd/include -Iuserland/libipc/include \
  tests/filed_tmpfs_page_alloc_unit.c -o "$out/unit"
"$out/unit"
