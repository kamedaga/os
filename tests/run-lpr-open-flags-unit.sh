#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/.artifacts/tests/lpr-open-flags"
mkdir -p "$out"
cc -std=c11 -Wall -Wextra -Werror -I"$root" \
  "$root/tests/lpr_open_flags_unit.c" -o "$out/test"
"$out/test"
echo "LPR_OPEN_FLAGS_UNIT=OK"
