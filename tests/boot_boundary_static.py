#!/usr/bin/env python3
"""Static guardrails for the storage-only bootfs and service-owned modules."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]

EXPECTED_BOOTFS = {
    "/srv/storage_boot.elf",
    "/srv/kobox/nvme-auth.ko",
    "/srv/kobox/nvme-core.ko",
    "/srv/kobox/nvme.ko",
    "/srv/kobox/crc16.ko",
    "/srv/kobox/mbcache.ko",
    "/srv/kobox/jbd2.ko",
    "/srv/kobox/ext4.ko",
}

MODULE_ORDERS = {
    "userland/storage_boot/src/main.c": [
        "nvme-auth.ko", "nvme-core.ko", "nvme.ko", "crc16.ko",
        "mbcache.ko", "jbd2.ko", "ext4.ko",
    ],
    "userland/netd/src/module_stack.c": [
        "virtio.ko", "virtio_ring.ko", "virtio_pci.ko", "failover.ko",
        "net_failover.ko", "virtio_net.ko",
    ],
    "userland/termd/src/linux_tty_island.c": [
        "linux_virtio.ko", "linux_virtio_ring.ko",
        "linux_virtio_pci_modern_dev.ko", "linux_virtio_pci_legacy_dev.ko",
        "linux_virtio_pci.ko", "linux_tty_core.ko", "linux_tty_n_null.ko",
        "linux_virtio_console.ko",
    ],
    "userland/drmd/src/drm_island.c": [
        "linux_virtio.ko", "linux_virtio_ring.ko",
        "linux_virtio_pci_modern_dev.ko", "linux_virtio_pci_legacy_dev.ko",
        "linux_virtio_pci.ko", "linux_virtio_dma_buf.ko",
        "linux_virtio_gpu.ko",
    ],
    "userland/inputd/src/input_island.c": [
        "linux_virtio.ko", "linux_virtio_ring.ko",
        "linux_virtio_pci_modern_dev.ko", "linux_virtio_pci_legacy_dev.ko",
        "linux_virtio_pci.ko", "linux_virtio_input.ko",
    ],
}


def assert_order(path: str, names: list[str]) -> None:
    text = (ROOT / path).read_text()
    positions = []
    for name in names:
        match = re.search(rf'"(?:/usr/lib/kobox/|/srv/kobox/)?{re.escape(name)}"', text)
        assert match is not None, f"{path}: missing {name}"
        positions.append(match.start())
    assert positions == sorted(positions), f"{path}: module order changed"


def main() -> None:
    pack = (ROOT / "pack/pack.yaml").read_text()
    bootfs = set(re.findall(r'^\s+bootfs:\s+"(/[^"]+)"\s*$', pack, re.MULTILINE))
    assert bootfs == EXPECTED_BOOTFS, f"bootfs mismatch: {sorted(bootfs)}"

    seed0boot = ROOT / "userland/seed0boot"
    for path in seed0boot.rglob("*"):
        if path.is_file() and path.suffix in {".c", ".h"}:
            assert ".ko" not in path.read_text(), f"module name leaked into {path}"

    for path, names in MODULE_ORDERS.items():
        assert_order(path, names)

    print("boot boundary static checks: OK")


if __name__ == "__main__":
    main()
