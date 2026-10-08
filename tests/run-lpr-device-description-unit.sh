#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-device-description
mkdir -p "$out"
includes=(userland/personality/include musl/pachaos/include userland/libpacha/include
  userland/libipc/include userland/filed/include userland/lpr_supervisor/include
  userland/netd/include userland/unixd/include userland/termd/include
  userland/gpud/include userland/inputd/include kobox2/protocol/include
  kobox2/protocol/generated/include)
flags=()
for directory in "${includes[@]}"; do flags+=("-I$directory"); done
clang -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  "${flags[@]}" tests/lpr_device_description_unit.c \
  userland/personality/linux/runtime/lpr_fd/device.c -Wl,--gc-sections \
  -o "$out/unit"
"$out/unit"
