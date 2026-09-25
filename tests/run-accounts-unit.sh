#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/accounts"
mkdir -p "$out"
clang -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$repo_root/userland/libaccount/include" \
  "$repo_root/tests/accounts_unit.c" "$repo_root/userland/libaccount/src/account.c" -o "$out/unit"
"$out/unit" "$repo_root/userland/fixtures/base/passwd" "$repo_root/userland/fixtures/base/group"
clang -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$repo_root/userland/libaccount/include" \
  -I"$repo_root/userland/libipc/include" -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/userland/filed/include" \
  "$repo_root/tests/accounts_filed_unit.c" "$repo_root/userland/libaccount/src/account.c" -o "$out/filed"
"$out/filed"
