#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$repo_root/.artifacts/fs-queue-unit}"
mkdir -p "$out"
protocol="$repo_root/kobox2/protocol"
for mode in normal profile; do
flags=()
if [[ "$mode" == profile ]]; then flags+=(-DPH_FS_STORAGE_PROFILE=1); fi
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -pthread \
  "${flags[@]}" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$protocol/include" -I "$protocol/generated/include" \
  "$repo_root/tests/kobox2_fs_queue_unit.c" \
  "$repo_root/userland/kobox2_adapter/fs_queue.c" \
  "$repo_root/userland/kobox2_owner/fs_channel.c" \
  "$protocol/src/filesystem.c" "$protocol/src/protocol.c" \
  "$protocol/src/virtqueue.c" "$protocol/src/virtqueue_memory.c" \
  "$protocol/arch/x86_64/virtqueue_atomic.c" -o "$out/$mode-unit"
"$out/$mode-unit"
done
