#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/fs-channel-unit}"
mkdir -p "$out"
protocol="$repo_root/kobox2/protocol"
for mode in normal profile; do
flags=()
if [[ "$mode" == profile ]]; then flags+=(-DPH_FS_CHANNEL_PROFILE=1 -DPH_FS_CHANNEL_TRACE=1); fi
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g \
  "${flags[@]}" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$protocol/include" -I "$protocol/generated/include" \
  "$repo_root/tests/kobox2_fs_channel_unit.c" \
  "$repo_root/userland/kobox2_owner/fs_channel.c" \
  "$protocol/src/filesystem.c" "$protocol/src/protocol.c" \
  "$protocol/src/virtqueue.c" "$protocol/src/virtqueue_memory.c" \
  "$protocol/arch/x86_64/virtqueue_atomic.c" -o "$out/$mode-unit"
"$out/$mode-unit"
done
