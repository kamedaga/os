#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/lpr-vsyscall"
mkdir -p "$out"
"${CC:-/usr/bin/clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined \
    "$repo_root/tests/lpr_vsyscall_unit.c" \
    "$repo_root/userland/personality/linux/runtime/lpr_vsyscall.c" -o "$out/unit"
"$out/unit"
