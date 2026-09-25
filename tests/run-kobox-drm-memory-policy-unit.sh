#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-memory-policy
mkdir -p "$out"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/kobox_drm_memory_policy_unit.c -o "$out/unit"
"$out/unit"
