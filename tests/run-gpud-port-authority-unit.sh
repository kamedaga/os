#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-port-authority
mkdir -p "$out"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I userland/gpud/include -I userland/libfiled_client/include \
  -I userland/libpacha/include -I userland/libipc/include -I userland/libcapsule/include \
  -I kobox2/include -I kobox2/protocol/include -I kobox2/protocol/generated/include \
  -I kobox2/linux-sandbox/kobox tests/gpud_port_authority_unit.c \
  kobox2/protocol/src/resource_grant.c kobox2/protocol/src/sha256.c -o "$out/unit"
"$out/unit"
