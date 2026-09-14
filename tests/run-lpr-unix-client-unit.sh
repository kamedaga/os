#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-unix-client"
mkdir -p "$out_dir"
includes=()
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$module/include")
done
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
  "$repo_root/userland/unixd/src/client_wire.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_unix/cache.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_unix/mapping.c" \
  "$repo_root/userland/unixd/src/broker.c" "$repo_root/userland/unixd/src/rights.c" \
  "$repo_root/userland/unixd/src/transport.c" "$repo_root/userland/unixd/src/notify.c" \
  "$repo_root/userland/libipc/src/status.c" \
  "$repo_root/tests/lpr_unix_client_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
