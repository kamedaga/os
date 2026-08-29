#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-linux-mremap"
mkdir -p "$out_dir"

/usr/bin/clang \
  -std=c11 -O2 -Wall -Wextra -Werror \
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
  "$repo_root/userland/personality/linux/runtime/lpr_memory.c" \
  "$repo_root/userland/libipc/src/status.c" \
  "$repo_root/tests/lpr_linux_mremap_unit.c" \
  -o "$out_dir/unit"

"$out_dir/unit"
