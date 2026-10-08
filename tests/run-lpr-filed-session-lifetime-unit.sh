#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-filed-session-lifetime"
mkdir -p "$out_dir"
runtime_source="${LPR_RETIREMENT_SOURCE_ROOT:-$repo_root/userland/personality/linux/runtime}"
protocol="$repo_root/kobox2/protocol"
includes=("-I$repo_root/userland" "-I$protocol/include" "-I$protocol/generated/include")
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$module/include")
done
for mode in normal diagnostic; do
flags=()
if [[ "$mode" == diagnostic ]]; then flags+=(-DLPR_BOOT_DIAGNOSTICS=1); fi
${CC:-/usr/bin/clang} -std=c11 -O1 -g -Wall -Wextra -Werror "${flags[@]}" \
  -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  "${includes[@]}" -I"$repo_root/musl/pachaos/include" \
  -I"$repo_root/userland/personality/linux/runtime" \
  -I"$repo_root/userland/personality/linux/runtime/lpr_vfs" \
  "$repo_root/tests/lpr_filed_session_lifetime_unit.c" \
  "$runtime_source/lpr_vfs/session.c" \
  "$runtime_source/lpr_common/runtime_support.c" \
  "$repo_root/userland/kobox2_owner/fs_channel.c" \
  "$protocol/src/filesystem.c" "$protocol/src/protocol.c" \
  "$protocol/src/virtqueue.c" "$protocol/src/virtqueue_memory.c" \
  "$protocol/arch/x86_64/virtqueue_atomic.c" -o "$out_dir/$mode-unit"
"$out_dir/$mode-unit"
done
if strings "$out_dir/normal-unit" | rg '\[LPR_BOOT\]|fs\.(hello|attach|notify|receive|queue)\.'; then
  echo 'Filesystem diagnostics leaked into default-OFF ELF' >&2
  exit 1
fi
