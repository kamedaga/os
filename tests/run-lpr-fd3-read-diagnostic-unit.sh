#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-fd3-read-diagnostic
mkdir -p "$out"
linker=lld
if [[ "${CC:-clang}" == *gcc* ]]; then linker=bfd; fi
includes=(-Iuserland -Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Imusl/pachaos/include)
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
for mode in normal diagnostic; do
  flags=()
  if [[ "$mode" == diagnostic ]]; then flags+=(-DLPR_BOOT_DIAGNOSTICS=1); fi
  ${CC:-/usr/bin/clang} -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" "${includes[@]}" \
    -flto -fuse-ld="$linker" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tests/lpr_fd3_read_diagnostic_unit.c -o "$out/$mode-unit"
  "$out/$mode-unit"
done
if strings "$out/normal-unit" | rg '\[LPR_BOOT\]|fd3.read.enter'; then exit 1; fi
if nm "$out/normal-unit" | rg 'lpr_boot_diagnostic'; then exit 1; fi
