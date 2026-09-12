#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-irq-queue-unit/${CC:-cc}"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$repo_root/tests/kobox2_irq_queue_unit.c" \
  "$repo_root/userland/kobox2_adapter/device_irq_queue.c" -o "$out/irq-queue-unit"
"$out/irq-queue-unit" | tee "$out/result.log"
sha256sum "$repo_root/tests/kobox2_irq_queue_unit.c" \
  "$repo_root/tests/run-kobox2-irq-queue-unit.sh" \
  "$repo_root/userland/kobox2_adapter/device_irq_queue.c" \
  "$repo_root/userland/kobox2_adapter/device_irq_queue.h" >"$out/inputs.sha256"
