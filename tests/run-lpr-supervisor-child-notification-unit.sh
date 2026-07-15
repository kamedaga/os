#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

out=".artifacts/tests/lpr_supervisor_child_notification_unit"
mkdir -p "$(dirname "$out")"
/usr/bin/clang \
  -std=c11 \
  -O2 \
  -ffunction-sections \
  -fdata-sections \
  -Iuserland/lpr_supervisor/include \
  -Iuserland/libipc/include \
  -Iuserland/libpacha/include \
  tests/lpr_supervisor_child_notification_unit.c \
  userland/libipc/src/status.c \
  -Wl,--gc-sections \
  -o "$out"
"$out"
