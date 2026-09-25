#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
lock="$repo_root/tools/manifests/alpine-kobox-kernel-v3.22-x86_64.lock"
cache="$repo_root/.artifacts/third_party/alpine-kobox-kernel-v3.22-x86_64"
output="$repo_root/.artifacts/alpine-kernel-modules"

python3 "$repo_root/pack/scripts/extract_alpine_modules.py" \
    --lock "$lock" \
    --cache "$cache" \
    --output "$output" \
    --package linux-lts \
    --module crc16=crc16.ko \
    --module mbcache=mbcache.ko \
    --module jbd2=jbd2.ko \
    --module ext4=ext4.ko \
    --module virtio=virtio.ko \
    --module virtio-ring=virtio_ring.ko \
    --module virtio-pci-modern-dev=virtio_pci_modern_dev.ko \
    --module virtio-pci-legacy-dev=virtio_pci_legacy_dev.ko \
    --module virtio-pci=virtio_pci.ko \
    --module virtio-console=virtio_console.ko \
    --module virtio-dma-buf=virtio_dma_buf.ko \
    --module virtio-gpu=virtio-gpu.ko \
    --module virtio-input=virtio_input.ko
