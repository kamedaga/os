#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${LPR_MMAP_TEST_OUTPUT:-$repo_root/.artifacts/tests/lpr-private-mmap-authority}"
source_root="${LPR_MMAP_SOURCE_ROOT:-$repo_root/userland/personality/linux/runtime}"
mkdir -p "$out"
cc="${CC:-/usr/bin/clang}"
linker=lld
if [[ "$cc" == *gcc* ]]; then linker=bfd; fi
includes=("-I$repo_root/userland" "-I$repo_root/kobox2/protocol/include" "-I$repo_root/kobox2/protocol/generated/include")
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$module/include")
done
includes+=("-I$repo_root/musl/pachaos/include" "-I$repo_root/userland/personality/linux/runtime")
includes+=("-I$repo_root/userland/personality/linux/runtime/lpr_fd")
for mode in normal diagnostic; do
  flags=()
  if [[ "$mode" == diagnostic ]]; then flags+=(-DLPR_BOOT_DIAGNOSTICS=1); fi
  if [[ -n "${LPR_MMAP_OLD_SOURCE:-}" ]]; then flags+=(-DLPR_MMAP_OLD_SOURCE=1); fi
  common=(-D_GNU_SOURCE -std=c11 -O2 -g -Wall -Wextra -Werror -flto -ffunction-sections -fdata-sections -fsanitize=address,undefined)
  # Retain actual public mmap/pin dispatch. Stat itself is a fixture input;
  # rename only its unused wrapper to keep the focused native VM model small.
  "$cc" "${common[@]}" "${flags[@]}" "${includes[@]}" -Dlpr_linux_fstat=lpr_unit_unused_fstat \
    -c "$repo_root/userland/personality/linux/runtime/lpr_fd/ops.c" -o "$out/$mode-ops.o"
  "$cc" "${common[@]}" "${flags[@]}" "${includes[@]}" \
    "$repo_root/tests/lpr_private_mmap_authority_unit.c" \
    "$source_root/lpr_dispatch.c" "$source_root/lpr_fd/metadata.c" \
    "$repo_root/userland/personality/linux/runtime/lpr_vfs/rpc.c" \
    "$repo_root/kobox2/protocol/src/filesystem.c" "$repo_root/userland/libipc/src/status.c" \
    "$out/$mode-ops.o" -fuse-ld="$linker" -Wl,--gc-sections -o "$out/$mode-unit"
  "$out/$mode-unit"
done
if strings "$out/normal-unit" | rg '\[LPR_BOOT\]|fs\.(rpc|queue|control)\.'; then
  echo 'Filesystem diagnostics leaked into default-OFF ELF' >&2; exit 1
fi
