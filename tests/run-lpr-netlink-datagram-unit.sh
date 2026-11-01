#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/unity-il2cpp-smoke/lpr-netlink-unit
mkdir -p "$out"
clang -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 -Iuserland/netd/include -Iuserland/libpacha/include -Iuserland/libipc/include \
 -Iuserland/libcapsule/include tests/lpr_netlink_datagram_unit.c -o "$out/unit"
"$out/unit"
