#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/.artifacts/tests/kobox2-wifi-service-check"
mkdir -p "$out"
/usr/bin/clang -std=c11 -Wall -Wextra -Werror \
    -I "$root/kobox2/linux-sandbox/kobox" \
    "$root/tests/kobox2_wifi_policy_unit.c" -o "$out/policy-unit"
"$out/policy-unit"
