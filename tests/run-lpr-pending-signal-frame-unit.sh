#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

out=".artifacts/tests/lpr_pending_signal_frame_unit"
mkdir -p "$(dirname "$out")"
/usr/bin/clang \
  -std=c11 \
  -O2 \
  -ffunction-sections \
  -fdata-sections \
  -Iuserland/personality/include \
  -Imusl/pachaos/include \
  -Iuserland/libpacha/include \
  -Iuserland/libipc/include \
  -Iuserland/filed/include \
  -Iuserland/lpr_supervisor/include \
  -Iuserland/netd/include \
  -Iuserland/termd/include \
  -Iuserland/drmd/include \
  -Iuserland/inputd/include \
  -Iuserland/personality/linux/hde \
  tests/lpr_pending_signal_frame_unit.c \
  -Wl,--gc-sections \
  -o "$out"
"$out"
