#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-filed-session-lifetime"
mkdir -p "$out_dir"

/usr/bin/clang \
  -std=c11 -O2 -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -pthread \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/userland/personality/include" \
  -I"$repo_root/musl/pachaos/include" \
  -I"$repo_root/userland/daemons/common/include" \
  -I"$repo_root/userland/filed/include" \
  -I"$repo_root/userland/termd/include" \
  -I"$repo_root/userland/drmd/include" \
  -I"$repo_root/userland/inputd/include" \
  -I"$repo_root/userland/netd/include" \
  -I"$repo_root/userland/lpr_supervisor/include" \
  "$repo_root/userland/personality/linux/runtime/lpr_common/runtime_support.c" \
  "$repo_root/tests/lpr_filed_session_lifetime_unit.c" \
  -Wl,--gc-sections \
  -o "$out_dir/unit"

"$out_dir/unit"
