#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/tests
includes=(-Iuserland -Imusl/pachaos/include -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include)
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
clang -std=c11 -O2 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections "${includes[@]}" \
  tests/lpr_clock_entry_unit.c tests/lpr_clock_entry_unit.S \
  userland/personality/linux/runtime/lpr_clock_copy.S -Wl,--gc-sections \
  -o .artifacts/tests/lpr-clock-entry-unit
.artifacts/tests/lpr-clock-entry-unit
clang -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin \
  -ffunction-sections -fdata-sections "${includes[@]}" \
  tests/lpr_entry_single_cache_unit.c userland/personality/linux/runtime/support/string.c -Wl,--gc-sections \
  -o .artifacts/tests/lpr-entry-single-cache-unit
.artifacts/tests/lpr-entry-single-cache-unit
clang -std=c11 -D_GNU_SOURCE -DLPR_ENTRY_IDENTITY_TEST -DLPR_THREAD_PAGE_CPU_TEST -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections "${includes[@]}" \
  tests/lpr_entry_identity_unit.c tests/lpr_entry_identity_unit.S \
  userland/personality/linux/runtime/lpr_entry.S -Wl,--gc-sections \
  -o .artifacts/tests/lpr-entry-identity-unit
.artifacts/tests/lpr-entry-identity-unit
