#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/filecache-tests
includes=(-Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Iuserland)
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
"${CC:-/usr/bin/clang}" -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -Imusl/pachaos/include \
  tests/lpr_statx_mask_unit.c -o .artifacts/filecache-tests/statx-mask
.artifacts/filecache-tests/statx-mask
