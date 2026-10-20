#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/kernel-memory-unit
mkdir -p "$out"
cc -O2 -Wall -Wextra -Werror -fno-builtin -mgeneral-regs-only tests/kernel_memory_unit.c -o "$out/test"
"$out/test"
