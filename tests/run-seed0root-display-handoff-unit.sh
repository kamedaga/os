#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/seed0root-display-handoff
vterm=.artifacts/third_party/libvterm-0.3.3/build
mkdir -p "$out"
includes=(-I"$vterm" -I"$vterm/include" -Ikobox2/protocol/include -Ikobox2/protocol/generated/include)
for module in libpacha libipc libcapsule inputd netd termd; do includes+=("-Iuserland/$module/include"); done
${CC:-clang} -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
 -Wno-missing-field-initializers -Wno-sign-compare \
 -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined \
 "${includes[@]}" tests/seed0root_display_handoff_unit.c \
 userland/libipc/src/ipc.c userland/libipc/src/status.c userland/libcapsule/src/capsule.c \
 "$vterm"/src/{encoding,keyboard,mouse,parser,pen,screen,state,unicode,vterm}.c -o "$out/unit"
"$out/unit"
