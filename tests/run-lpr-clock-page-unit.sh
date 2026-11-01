#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
out=".artifacts/tests/lpr_clock_page_unit"
mkdir -p "$(dirname "$out")"
/usr/bin/clang -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
  -Iuserland/personality/include -Imusl/pachaos/include \
  -Iuserland/libpacha/include -Iuserland/libipc/include \
  -Iuserland/filed/include -Iuserland/lpr_supervisor/include \
  -Iuserland/netd/include -Iuserland/unixd/include \
  -Iuserland/termd/include -Iuserland/gpud/include -Iuserland/inputd/include \
  -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  tests/lpr_clock_page_unit.c userland/personality/linux/runtime/lpr_clock_copy.S -Wl,--gc-sections -o "$out"
"$out"
