#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/tests/gpud-discovery
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined \
  -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/libcapsule/include -Iuserland/filed/include \
  -Iuserland/libfiled_client/include -Ikobox2/protocol/include \
  -Ikobox2/protocol/generated/include \
  tests/gpud_discovery_unit.c kobox2/protocol/src/filesystem.c \
  -o .artifacts/tests/gpud-discovery/unit
.artifacts/tests/gpud-discovery/unit
