#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/gpud-gpu-query-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/gpud_gpu_query_unit.c" "$repo_root/userland/gpud/drm_translate.c"
  "$repo_root/userland/gpud/drm_reply.c" "$repo_root/kobox2/protocol/src/protocol.c"
  "$repo_root/userland/kobox2_adapter/gpu_query.c"
  "$repo_root/userland/kobox2_adapter/gpu_session_service.c"
  "$repo_root/userland/gpud/gpu_sessions.c" "$repo_root/kobox2/protocol/src/gpu_session.c"
  "$repo_root/userland/kobox2_adapter/gpu_queue.c" "$repo_root/userland/gpud/gpu_channel.c"
  "$repo_root/kobox2/protocol/src/virtqueue.c" "$repo_root/kobox2/protocol/src/virtqueue_memory.c"
  "$repo_root/kobox2/protocol/arch/x86_64/virtqueue_atomic.c"
  "$repo_root/kobox2/linux-sandbox/kobox/boot/drm_query.c"
  "$repo_root/kobox2/protocol/src/gpu.c" "$repo_root/kobox2/protocol/src/gpu_completion.c")
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/protocol/include" -I "$repo_root/kobox2/protocol/generated/include" \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  -isystem "$repo_root/userland/drmd/include" -isystem "$repo_root/userland/libipc/include" \
  -isystem "$repo_root/userland/libpacha/include" \
  "${sources[@]}" -o "$out/gpu-query-unit"
"$out/gpu-query-unit" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/gpud/drm_translate.h" \
  "$repo_root/kobox2/linux-sandbox/kobox/boot/drm_query.h" \
  "$repo_root/kobox2/linux-sandbox/kobox/boot/drm_file.h" \
  "$repo_root/kobox2/protocol/include/kobox2/gpu.h" \
  "$repo_root/userland/gpud/drm_reply.h" \
  "$repo_root/userland/kobox2_adapter/gpu_query.h" \
  "$repo_root/userland/kobox2_adapter/gpu_session_service.h" \
  "$repo_root/userland/gpud/gpu_sessions.h" \
  "$repo_root/kobox2/protocol/include/kobox2/gpu_session.h" \
  "$repo_root/kobox2/linux-sandbox/kobox/boot/drm_service.h" \
  "$repo_root/userland/kobox2_adapter/gpu_query_message.h" \
  "$repo_root/userland/kobox2_adapter/gpu_queue.h" \
  "$repo_root/userland/kobox2_adapter/gpu_queue_message.h" \
  "$repo_root/userland/gpud/gpu_channel.h" \
  "$repo_root/kobox2/protocol/include/kobox2/virtqueue.h" \
  "$repo_root/kobox2/protocol/include/kobox2/virtqueue_memory.h" \
  "$repo_root/kobox2/protocol/include/kobox2/virtqueue_x86_64.h" \
  "$repo_root/tests/run-gpud-gpu-query-unit.sh" >"$out/inputs.sha256"
