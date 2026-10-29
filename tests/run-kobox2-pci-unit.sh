#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-pci-unit"
mkdir -p "$out"
for profile in default nec-high; do
extra=()
suffix=""
if [[ "$profile" == nec-high ]]; then
  # NEC Tiger Lake xHCI BARs live around 413 GiB, well above the older
  # fixture's 4.5 GiB aperture. Exercise the actual address width too.
  extra=(-DBAR_BASE=0x603d160000ULL)
  suffix=-nec-high
fi
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libcapsule/include" -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "${extra[@]}" "$repo_root/tests/kobox2_pci_unit.c" -o "$out/pci-unit$suffix"
"$out/pci-unit$suffix" | tee "$out/result$suffix.log"
done
sha256sum "$repo_root/userland/kobox2_adapter/device_pci.c" \
  "$repo_root/userland/kobox2_adapter/device_pci.h" \
  "$repo_root/tests/kobox2_pci_unit.c" "$repo_root/tests/run-kobox2-pci-unit.sh" \
  >"$out/inputs.sha256"
