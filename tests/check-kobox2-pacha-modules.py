#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check exactly the core and native module files selected by a bootfs manifest."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--profile", choices=("gem", "virtio"), default="gem")
    args = parser.parse_args()
    entries = {}
    for line in args.manifest.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        destination, source = line.split("=", 1)
        if destination in entries:
            raise ValueError(f"duplicate bootfs entry: {destination}")
        entries[destination] = (args.manifest.parent / source).resolve(strict=True)
    inputs = json.loads(args.inputs.read_text())
    core = entries["/srv/kobox2/core.so"]
    if digest(core) != inputs["core_sha256"]:
        raise ValueError("native modules were built for a different core")
    records = {}
    for record in inputs["modules"]:
        name = Path(record["path"]).name
        if name in records:
            raise ValueError(f"ambiguous module basename: {name}")
        records[name] = record
    selected = {name: source for name, source in entries.items()
                if name.startswith("/srv/kobox2/modules/")}
    required = {"i2c-core.ko", "drm_panel_orientation_quirks.ko", "drm.ko",
                "drm_shmem_helper.ko"}
    if args.profile == "gem":
        required.add("lifetime_test.ko")
    else:
        required.update({"virtio.ko", "virtio_ring.ko", "virtio_pci_modern_dev.ko",
                         "virtio_pci.ko", "virtio_dma_buf.ko", "drm_kms_helper.ko",
                         "virtio-gpu.ko"})
    if not required.issubset(Path(name).name for name in selected):
        raise ValueError(f"incomplete native {args.profile} module set")
    for destination, source in sorted(selected.items()):
        expected = records[Path(destination).name]["sha256"]
        actual = digest(source)
        if actual != expected:
            raise ValueError(f"module does not match strict-modpost inputs: {destination}")
        print(f"{actual}  {source}")
    print(f"{digest(args.inputs)}  {args.inputs.resolve()}")


if __name__ == "__main__":
    main()
