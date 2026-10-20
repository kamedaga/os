#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/kernel-refactor-stage3/fdlimit-lpr-fd-growth.elf}"
mkdir -p "$(dirname "$out")"
"${MUSL_CC:-musl-gcc}" -O2 -Wall -Wextra -Werror -static \
  "$repo_root/tests/lpr_fd_growth_guest.c" -o "$out"
