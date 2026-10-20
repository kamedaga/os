#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-tty-fork-page
mkdir -p "$out"
includes=(-Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Iuserland)
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
clang -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -Imusl/pachaos/include \
  tests/lpr_tty_fork_page_unit.c \
  userland/personality/linux/runtime/lpr_vfs/path.c -o "$out/unit"
timeout 15s "$out/unit"
