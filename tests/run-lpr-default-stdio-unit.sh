#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-default-stdio
mkdir -p "$out"
includes=()
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
${CC:-/usr/bin/clang} -std=c11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -Imusl/pachaos/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  tests/lpr_default_stdio_unit.c -o "$out/unit"
"$out/unit"
${CC:-/usr/bin/clang} -std=c11 -O2 -g -Wall -Wextra -Werror \
  -DLPR_BOOT_DIAGNOSTICS=1 \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -Imusl/pachaos/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  tests/lpr_default_stdio_unit.c tests/lpr_boot_diagnostic_other_tu.c -o "$out/diagnostic-unit"
"$out/diagnostic-unit"
if strings "$out/unit" | rg '\[LPR_BOOT\]|stdio\.(open|dup|snapshot)|tty\.(call|recv)'; then
  echo 'Boot diagnostics leaked into default-OFF ELF' >&2
  exit 1
fi
