#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-irq-unit/${CC:-cc}"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libcapsule/include" -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$repo_root/tests/kobox2_irq_unit.c" \
  "$repo_root/userland/kobox2_adapter/device_irq_queue.c" -o "$out/irq-unit"
"$out/irq-unit" | tee "$out/result.log"
sha256sum "$repo_root/userland/kobox2_adapter/device_irq.c" \
  "$repo_root/userland/kobox2_adapter/device_irq.h" \
  "$repo_root/userland/kobox2_adapter/device_irq_queue.c" \
  "$repo_root/userland/kobox2_adapter/device_irq_queue.h" \
  "$repo_root/tests/kobox2_irq_unit.c" "$repo_root/tests/run-kobox2-irq-unit.sh" \
  >"$out/inputs.sha256"
