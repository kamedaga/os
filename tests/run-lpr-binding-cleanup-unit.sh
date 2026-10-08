#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/lpr-binding-cleanup"
mkdir -p "$out"
includes=("-I$repo_root/kobox2/protocol/include" "-I$repo_root/kobox2/protocol/generated/include")
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$module/include")
done
for mode in normal diagnostic; do
  flags=()
  if [[ "$mode" == diagnostic ]]; then flags+=(-DLPR_BOOT_DIAGNOSTICS=1); fi
  ${CC:-/usr/bin/clang} -std=c11 -O1 -g -Wall -Wextra -Werror "${flags[@]}" \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
    "$repo_root/tests/lpr_binding_cleanup_unit.c" \
    "$repo_root/userland/personality/linux/runtime/lpr_fd/control.c" \
    "$repo_root/userland/personality/linux/runtime/lpr_fd/table.c" \
    "$repo_root/userland/filed/src/exec_payload.c" \
    "$repo_root/kobox2/protocol/src/filesystem.c" -o "$out/$mode-unit"
  "$out/$mode-unit"
  ${CC:-/usr/bin/clang} -std=c11 -O1 -g -Wall -Wextra -Werror "${flags[@]}" \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
    "$repo_root/tests/lpr_dup_binding_unit.c" \
    "$repo_root/userland/personality/linux/runtime/lpr_fd/table.c" -o "$out/$mode-dup-unit"
  "$out/$mode-dup-unit"
done
