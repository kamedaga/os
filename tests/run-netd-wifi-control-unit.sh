#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/tests/netd-wifi-control-unit"
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -I"$root/userland/netd/include" -I"$root/userland/netd/src" \
  -I"$root/userland/libpacha/include" -I"$root/userland/libipc/include" \
  -I"$root/kobox2/linux-sandbox/kobox" \
  "$root/tests/netd_wifi_control_unit.c" -o "$out/test"
sha256sum "$root/userland/netd/src/wifi_control.c" \
  "$root/tests/netd_wifi_control_unit.c" > "$out/inputs.sha256"
timeout 10 "$out/test" 2>&1 | tee "$out/result.log"
