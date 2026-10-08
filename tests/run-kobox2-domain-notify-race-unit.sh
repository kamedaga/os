#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-domain-notify-race"
domain="$repo_root/kobox2/linux-sandbox/kobox/machine"
mkdir -p "$out"
for cc in gcc clang; do
  "$cc" -std=c11 -O2 -Wall -Wextra -Werror \
    "$domain/domain.c" "$domain/domain_test.c" -o "$out/domain-$cc"
  "$out/domain-$cc"
  "$cc" -std=c11 -O2 -Wall -Wextra -Werror -pthread \
    "$domain/domain.c" "$repo_root/tests/kobox2_domain_notify_race_unit.c" -o "$out/race-$cc"
  timeout 10s "$out/race-$cc"
done
