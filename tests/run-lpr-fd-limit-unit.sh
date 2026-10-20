#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-fd-limit"
mkdir -p "$out_dir"

/usr/bin/clang \
  -std=c11 -O1 -g -pthread -fsanitize=address,undefined -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/userland/personality/include" \
  -I"$repo_root/musl/pachaos/include" \
  -I"$repo_root/userland/daemons/common/include" \
  -I"$repo_root/userland/filed/include" \
  -I"$repo_root/userland/termd/include" \
  -I"$repo_root/userland/gpud/include" \
  -I"$repo_root/userland/inputd/include" \
  -I"$repo_root/userland/netd/include" \
  -I"$repo_root/userland/unixd/include" \
  -I"$repo_root/userland/lpr_supervisor/include" \
  -I"$repo_root/userland" \
  -I"$repo_root/kobox2/protocol/include" \
  -I"$repo_root/kobox2/protocol/generated/include" \
  "$repo_root/tests/lpr_fd_limit_unit.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/table.c" \
  "$repo_root/userland/personality/src/lpr_manifest.c" \
  -o "$out_dir/unit"

"$out_dir/unit"
