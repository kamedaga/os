#!/usr/bin/env python3
"""Static guardrails for the storage-only bootfs and service-owned modules."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]

STORAGE_MANIFEST = "userland/storage/include/storage/stack.def"
STORAGE_CONSUMERS = (
    "userland/storage_boot/src/main.c",
    "userland/seed0root/src/main.c",
    "userland/koboxd/src/bootstrap.c",
    "userland/koboxd/src/storage_runtime.c",
)

MODULE_ORDERS = {
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
    manifest_text = (ROOT / STORAGE_MANIFEST).read_text()
    manifest = re.findall(
        r'^STORAGE_STACK_MODULE\([^,]+,\s*([^,]+),\s*"([^"]+)",\s*"([^"]+)"',
        manifest_text,
        re.MULTILINE,
    )
    assert len(manifest) == 7, f"storage manifest count changed: {len(manifest)}"
    phases = [phase.strip() for phase, _, _ in manifest]
    storage_names = [name for _, name, _ in manifest]
    paths = [path for _, _, path in manifest]
    assert len(set(storage_names)) == len(storage_names), "duplicate storage module name"
    assert len(set(paths)) == len(paths), "duplicate storage module path"
    assert phases == ["NVME"] * 3 + ["FILESYSTEM"] * 4, \
        f"storage phase/order changed: {phases}"

    expected_bootfs = {"/srv/storage_boot.elf", *paths}
    pack = (ROOT / "pack/pack.yaml").read_text()
    bootfs = set(re.findall(r'^\s+bootfs:\s+"(/[^"]+)"\s*$', pack, re.MULTILINE))
    assert bootfs == expected_bootfs, f"bootfs mismatch: {sorted(bootfs)}"

    seed0boot = ROOT / "userland/seed0boot"
    for path in seed0boot.rglob("*"):
        if path.is_file() and path.suffix in {".c", ".h"}:
            assert ".ko" not in path.read_text(), f"module name leaked into {path}"

    for path, module_names in MODULE_ORDERS.items():
        assert_order(path, module_names)

    for consumer in STORAGE_CONSUMERS:
        text = (ROOT / consumer).read_text()
        for name in storage_names:
            assert name not in text, f"storage module name duplicated in {consumer}: {name}"
        for path in paths:
            assert path not in text, f"storage module path duplicated in {consumer}: {path}"

    print("boot boundary static checks: OK")


if __name__ == "__main__":
    main()
