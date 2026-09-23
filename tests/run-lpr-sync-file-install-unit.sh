#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/lpr-sync-file-install"
mkdir -p "$out"
includes=()
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$module/include")
done
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined "${includes[@]}" \
  -I"$repo_root/musl/pachaos/include" \
  "${SYNC_FILE_SOURCE:-$repo_root/userland/personality/linux/runtime/lpr_sync_file.c}" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/table.c" \
  "${EVENT_FD_SOURCE:-$repo_root/userland/personality/linux/runtime/lpr_fd/dup_pipe.c}" \
  "$repo_root/tests/lpr_sync_file_install_unit.c" -o "$out/unit"
"$out/unit"
