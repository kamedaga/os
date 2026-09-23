#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=".artifacts/tests/kobox-nvme-dbbuf/${CC:-cc}"
mkdir -p "$out"
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -I_kobox/include -I_kobox/src \
    tests/kobox_nvme_dbbuf_unit.c _kobox/src/linux_subsystem/nvme/nvme.c \
    -Wl,--gc-sections -o "$out/unit"
"$out/unit"
# ASan's global registry keeps the unrelated NVMe driver operations table
# linked. Exercise the queue adapter separately with UBSan and section GC,
# without inventing successful stubs for disk/DMA operations.
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=undefined -fno-omit-frame-pointer -DKOBOX_NVME_QUEUE_TEST \
    -ffunction-sections -fdata-sections -I_kobox/include -I_kobox/src \
    tests/kobox_nvme_dbbuf_unit.c _kobox/src/linux_subsystem/nvme/nvme.c \
    -Wl,--gc-sections -o "$out/queue-unit"
"$out/queue-unit"
