#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out="${RESIDENT_TEST_OUT:-$root/.artifacts/vmo-memory-plan/stage4b/unit}"
mkdir -p "$out/include/linux"
# Mock only Linux facilities; compile the actual reporting.c and policy.
for h in export types mutex list oom errno atomic jiffies ktime swap gfp workqueue mm mmzone preempt rcupdate sched compiler_attributes; do
  : > "$out/include/linux/$h.h"
done
"${CC:-clang}" -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
 -fsanitize=address,undefined -I "$out/include" \
 "$root/userland/kobox2_adapter/tests/resident_unit.c" -o "$out/resident-unit"
ASAN_OPTIONS=detect_leaks=0 "$out/resident-unit"
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
 -I "$root/userland/libpacha/include" -I "$root/userland/libipc/include" \
 -I "$root/kobox2/linux-sandbox/kobox" -I "$root/kobox2/protocol/include" \
 -I "$root/kobox2/protocol/generated/include" \
 "$root/userland/kobox2_adapter/tests/resident_adapter_unit.c" \
 "$root/userland/libipc/src/ipc.c" -Wl,--gc-sections -o "$out/resident-adapter-unit"
ASAN_OPTIONS=detect_leaks=0 "$out/resident-adapter-unit"
