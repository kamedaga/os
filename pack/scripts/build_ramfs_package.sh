#!/bin/bash
# The same filesystem core/adapter, sealed without a PCI resource contract.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_tree="$repo_root/kobox2/linux-sandbox"
source_package="$repo_root/.artifacts/kobox2-storage-production"
output="$repo_root/.artifacts/kobox2-ramfs-production"
python3 "$source_tree/kobox/manifest/build_pci_package.py" \
  --inventory "$source_package/closure-inventory.json" \
  --runtime-dir "$source_package/runtime" --output-dir "$output/package" \
  --protocol-dir "$repo_root/kobox2/protocol" --profile storage --ram-root \
  --generator-dir "$output/manifest-build"
install -m 0555 "$source_package/sandbox.elf" "$output/package/sandbox.elf"
