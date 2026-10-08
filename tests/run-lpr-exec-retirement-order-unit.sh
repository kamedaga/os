#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-exec-retirement-order
mkdir -p "$out"
linker=lld
if [[ "${CC:-clang}" == *gcc* ]]; then linker=bfd; fi
includes=(-Iuserland -Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Imusl/pachaos/include)
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
for mode in 0 1; do
  ${CC:-/usr/bin/clang} -std=c11 -O1 -g -Wall -Wextra -Werror -DLPR_BOOT_DIAGNOSTICS="$mode" \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined \
    "${includes[@]}" tests/lpr_exec_retirement_order_unit.c \
    userland/personality/linux/runtime/lpr_fd/table.c -o "$out/unit-$mode"
  "$out/unit-$mode"
  ${CC:-/usr/bin/clang} -std=c11 -O2 -g -Wall -Wextra -Werror -DLPR_BOOT_DIAGNOSTICS="$mode" \
    -flto -fuse-ld="$linker" -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined "${includes[@]}" \
    tests/lpr_filed_terminal_binding_unit.c -o "$out/terminal-$mode"
  "$out/terminal-$mode"
done
