#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/.artifacts/tests/lpr-flock";mkdir -p "$out"
includes=(-I"$root/userland" -I"$root/kobox2/protocol/include" -I"$root/kobox2/protocol/generated/include" -I"$root/musl/pachaos/include")
for mod in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor;do includes+=(-I"$root/userland/$mod/include");done
${CC:-clang} -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -ffunction-sections -fdata-sections -Wl,--gc-sections "${includes[@]}" "$root/tests/lpr_flock_unit.c" "$root/userland/personality/linux/runtime/lpr_vfs/flock.c" -o "$out/test"
"$out/test"
