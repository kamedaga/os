#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-fs-boot-diagnostic
mkdir -p "$out"
includes=(-Iuserland -Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Imusl/pachaos/include)
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-Iuserland/$module/include")
done
for mode in normal diagnostic; do
  flags=()
  if [[ "$mode" == diagnostic ]]; then flags+=(-DLPR_BOOT_DIAGNOSTICS=1 -DPH_FS_CHANNEL_DIAGNOSTICS=1); fi
  ${CC:-/usr/bin/clang} -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" "${includes[@]}" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tests/lpr_fs_boot_diagnostic_unit.c \
    userland/personality/linux/runtime/lpr_vfs/rpc.c \
    userland/personality/linux/runtime/lpr_vfs/session.c \
    userland/kobox2_owner/fs_channel.c \
    kobox2/protocol/src/{filesystem,protocol,virtqueue,virtqueue_memory}.c \
    kobox2/protocol/arch/x86_64/virtqueue_atomic.c -o "$out/$mode-unit"
  "$out/$mode-unit"
done
if strings "$out/normal-unit" | rg '\[LPR_BOOT\]|fs\.(rpc|control|queue|notify|receive|hello|attach|supervisor|endpoint)\.'; then
  echo 'Filesystem boot diagnostics leaked into default-OFF ELF' >&2
  exit 1
fi
if nm "$out/normal-unit" | rg 'lpr_boot_diagnostic|ph_fs_channel_diagnostic'; then
  echo 'Filesystem boot diagnostic symbols leaked into default-OFF ELF' >&2
  exit 1
fi
