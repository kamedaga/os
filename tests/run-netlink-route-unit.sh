#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/netlink-route-unit"
mkdir -p "$out"
clang -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/userland/netd/include" -I"$root/userland/netd/src" \
  -I"$root/userland/libpacha/include" -I"$root/userland/libipc/include" \
  -I"$root/userland/libcapsule/include" \
  "$root/tests/netlink_route_unit.c" -o "$out/test"
"$out/test"
