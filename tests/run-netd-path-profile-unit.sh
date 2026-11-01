#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/netd-path-profile
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -Iuserland/libpacha/include -Iuserland/libipc/include -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Ikobox2/linux-sandbox/kobox \
 tests/netd_path_profile_unit.c -o "$out/unit"
"$out/unit"
