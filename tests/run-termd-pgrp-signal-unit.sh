#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

out=".artifacts/tests/termd_pgrp_signal_unit"
mkdir -p "$(dirname "$out")"
/usr/bin/clang \
  -std=c11 \
  -O2 \
  -ffunction-sections \
  -fdata-sections \
  -I_kobox/include \
  -I_kobox/src \
  -Iuserland/libcapsule/include \
  -Iuserland/libpacha/include \
  tests/termd_pgrp_signal_unit.c \
  _kobox/src/linux_personality/linux_stubs.c \
  -Wl,--gc-sections \
  -o "$out"
"$out"
