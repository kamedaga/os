#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/lpr-fs-rpc"
mkdir -p "$out"
runtime_source="${LPR_RPC_SOURCE_ROOT:-$repo_root/userland/personality/linux/runtime}"
includes=("-I$repo_root/userland" "-I$repo_root/kobox2/protocol/include" "-I$repo_root/kobox2/protocol/generated/include")
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$module/include")
done
for mode in normal diagnostic; do
flags=()
if [[ "$mode" == diagnostic ]]; then flags+=(-DLPR_BOOT_DIAGNOSTICS=1); fi
${CC:-/usr/bin/clang} -std=c11 -O1 -g -Wall -Wextra -Werror "${flags[@]}" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
  -I"$repo_root/userland/personality/linux/runtime/lpr_vfs" \
  -I"$repo_root/userland/personality/linux/runtime" \
  "$repo_root/tests/lpr_fs_rpc_unit.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_vfs/rpc.c" \
  "$runtime_source/lpr_vfs/ops.c" \
  "$runtime_source/lpr_vfs/io.c" \
  "$repo_root/kobox2/protocol/src/filesystem.c" -o "$out/$mode-unit"
"$out/$mode-unit"
done
if strings "$out/normal-unit" | rg '\[LPR_BOOT\]|\[fs-ring\]|fs\.(rpc|queue|control)\.'; then
  echo 'Filesystem diagnostics leaked into default-OFF ELF' >&2
  exit 1
fi
