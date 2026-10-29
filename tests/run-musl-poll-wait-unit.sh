#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/musl-poll-wait"
mkdir -p "$out"
"${CC:-/usr/bin/clang}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repo_root/tests/musl_poll_wait_unit.c" -o "$out/unit"
"$out/unit"
