#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/tests/kobox2-fault-return-unit"
mkdir -p "$out"
"${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I "$root/userland/libpacha/include" -I "$root/userland/libipc/include" \
  -I "$root/userland/personality/include" -I "$root/userland/personality/linux/runtime" \
  -I "$root/kobox2/linux-sandbox/kobox" \
  "$root/tests/kobox2_fault_return_unit.c" "$root/userland/kobox2_adapter/vm_client.c" \
  -o "$out/test"
"$out/test" | tee "$out/result.log"
