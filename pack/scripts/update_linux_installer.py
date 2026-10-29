#!/usr/bin/env python3
"""Grow an existing installer ISO and emit a USB patch, preserving ROOTFS.TAR."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

REPO = Path(__file__).resolve().parents[2]
SECTOR = 2048


def root_extent(image):
    result = subprocess.run(["xorriso", "-indev", str(image), "-find", "/pacha/ROOTFS.TAR",
                             "-exec", "report_lba", "--"], capture_output=True, text=True, check=True)
    matches = re.findall(r"File data lba:\s+\d+\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)", result.stdout)
    if len(matches) != 1:
        raise ValueError("Expected one contiguous ROOTFS.TAR extent.")
    start, blocks, length = map(int, matches[0])
    return {"offset": start*SECTOR, "bytes": blocks*SECTOR, "file_bytes": length}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--expect-sha256", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--map", nargs=2, action="append", metavar=("SOURCE", "ISO_PATH"),
                        help="Replace only these files instead of the default installer scripts")
    args = parser.parse_args()
    image = args.image.resolve(strict=True)
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest_path, patch_path = out/"usb-patch.json", out/"usb-patch.bin"
    if manifest_path.exists() or patch_path.exists():
        parser.error("Use a new output directory; do not overwrite an earlier patch.")
    extent = root_extent(image)
    before_bytes = image.stat().st_size
    before_hash, sectors = hashlib.sha256(), bytearray()
    with image.open("rb") as stream:
        while data := stream.read(SECTOR):
            before_hash.update(data)
            sectors.extend(hashlib.sha256(data).digest())
    if before_hash.hexdigest() != args.expect_sha256:
        parser.error("Existing ISO checksum mismatch; image was not changed.")
    (out/"before-sectors.sha256.bin").write_bytes(sectors)
    (out/"before.json").write_text(json.dumps({"bytes": before_bytes,
        "sha256": before_hash.hexdigest(), "rootfs_extent": extent}, indent=2)+"\n")
    print("Growing ISO in place; ROOTFS.TAR remains at its original location.", flush=True)
    command = ["xorriso", "-abort_on", "SORRY", "-dev", str(image), "-boot_image", "any", "replay"]
    source = REPO/"pack/linux-installer"
    mappings = args.map or [(str(source/name), target) for name, target in (("pacha-install", "/pacha/pacha-install"),
                         ("pacha-install-plan.py", "/pacha/pacha-install-plan.py"),
                         ("autorun", "/autorun/pacha-setup"))]
    for path, target in mappings:
        if not target.startswith("/") or target == "/pacha/ROOTFS.TAR":
            parser.error("Invalid or protected ISO path.")
        command += ["-map", str(Path(path).resolve(strict=True)), target]
    executable = [target for _, target in mappings if target in ("/pacha/pacha-install", "/autorun/pacha-setup")]
    if executable:
        command += ["-chmod", "0755", *executable, "--"]
    command += ["-commit"]
    subprocess.run(command, check=True)
    if root_extent(image) != extent:
        raise ValueError("ROOTFS.TAR moved; refusing to emit a USB patch.")
    ranges, after_hash = [], hashlib.sha256()
    with image.open("rb") as stream, patch_path.open("xb") as patch:
        index = 0
        while data := stream.read(SECTOR):
            after_hash.update(data)
            old = sectors[index*32:(index+1)*32]
            if hashlib.sha256(data).digest() != old:
                offset = index*SECTOR
                if offset < extent["offset"]+extent["bytes"] and offset+len(data) > extent["offset"]:
                    raise ValueError("ROOTFS.TAR data changed; refusing to emit a USB patch.")
                if ranges and ranges[-1]["offset"]+ranges[-1]["bytes"] == offset:
                    ranges[-1]["bytes"] += len(data)
                else:
                    ranges.append({"offset": offset, "bytes": len(data), "patch_offset": patch.tell()})
                patch.write(data)
            index += 1
    sha = after_hash.hexdigest()
    with patch_path.open("rb") as stream:
        patch_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = {"image": str(image), "before_bytes": before_bytes,
                "before_sha256": before_hash.hexdigest(), "after_bytes": image.stat().st_size,
                "after_sha256": sha, "patch_bytes": patch_path.stat().st_size,
                "patch_sha256": patch_hash, "rootfs_extent": extent,
                "rootfs_unchanged": True, "ranges": ranges}
    manifest_path.write_text(json.dumps(manifest, indent=2)+"\n")
    image.with_suffix(".iso.sha256").write_text(f"{sha}  {image.name}\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
