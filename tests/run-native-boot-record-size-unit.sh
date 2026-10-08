#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/native-boot-record-size
mkdir -p "$out"
includes=(-Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Ikobox2/linux-sandbox/kobox -I_kobox/include -I_kobox/src)
for module in libipc libpacha libcapsule personality daemons/common storage_boot pachaos_capsule filed termd gpud usbd inputd netd logd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" tests/native_boot_record_size_unit.c -o "$out/unit"
"$out/unit"
