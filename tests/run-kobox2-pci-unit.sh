#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-pci-unit"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libcapsule/include" -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$repo_root/tests/kobox2_pci_unit.c" -o "$out/pci-unit"
"$out/pci-unit" | tee "$out/result.log"
sha256sum "$repo_root/userland/kobox2_adapter/device_pci.c" \
  "$repo_root/userland/kobox2_adapter/device_pci.h" \
  "$repo_root/tests/kobox2_pci_unit.c" "$repo_root/tests/run-kobox2-pci-unit.sh" \
  >"$out/inputs.sha256"
