#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-utimens-plan"
cc="${CC:-/usr/bin/clang}"

mkdir -p "$out_dir"

"$cc" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -ffunction-sections \
  -fdata-sections \
  -Wl,--gc-sections \
  -I"$repo_root/userland/personality/include" \
  -I"$repo_root/musl/pachaos/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/filed/include" \
  -I"$repo_root/userland/lpr_supervisor/include" \
  -I"$repo_root/userland/netd/include" \
  -I"$repo_root/userland/termd/include" \
  -I"$repo_root/userland/drmd/include" \
  -I"$repo_root/userland/inputd/include" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/metadata.c" \
  "$repo_root/tests/lpr_utimens_plan_unit.c" \
  -o "$out_dir/lpr_utimens_plan_unit"

"$out_dir/lpr_utimens_plan_unit"
