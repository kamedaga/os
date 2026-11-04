#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/epiphany-slow/tests";mkdir -p "$out"
inc=(-I"$root/userland" -I"$root/kobox2/protocol/include" -I"$root/kobox2/protocol/generated/include" -I"$root/musl/pachaos/include")
for m in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor;do inc+=(-I"$root/userland/$m/include");done
clang -I"$root/userland/personality/linux/runtime" -I"$root/userland/personality/linux/runtime/lpr_vfs" -std=gnu11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined "${inc[@]}" "$@" "$root/tests/lpr_proc_mounts_unit.c" -o "$out/proc-mounts"
"$out/proc-mounts"
