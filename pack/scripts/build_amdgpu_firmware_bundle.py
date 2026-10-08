#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a fixed-hash, read-only launch bundle for the AMDGPU sandbox."""

import argparse
import hashlib
import pathlib
import re
import struct

MAGIC = 0x314C444E424D5746
MAX_FILES = 64
MAX_FILE = 8 << 20
MAX_TOTAL = 64 << 20
HEADER_BYTES = 32 + 176 * MAX_FILES


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--firmware-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    entries = []
    for line in args.manifest.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        digest, name = line.split(maxsplit=1)
        name = name.strip()
        if (len(digest) != 64 or not re.fullmatch(r"[0-9a-f]{64}", digest) or
                len(name) > 127 or not name.startswith("amdgpu/") or
                any(not part or part in (".", "..") or
                    not re.fullmatch(r"[A-Za-z0-9_.-]+", part)
                    for part in name.split("/"))):
            raise ValueError(f"invalid pinned firmware entry: {name}")
        if any(previous[0] == name for previous in entries):
            raise ValueError(f"duplicate pinned firmware entry: {name}")
        content = (args.firmware_dir / name).read_bytes()
        if not content or len(content) > MAX_FILE or hashlib.sha256(content).hexdigest() != digest:
            raise ValueError(f"firmware content differs from pinned hash: {name}")
        entries.append((name, bytes.fromhex(digest), content))
    if not entries or len(entries) > MAX_FILES:
        raise ValueError("invalid firmware count")
    if sum(len(content) for _, _, content in entries) > MAX_TOTAL:
        raise ValueError("firmware bundle exceeds its size limit")

    payload = bytearray()
    descriptors = bytearray()
    for name, digest, content in entries:
        offset = HEADER_BYTES + len(payload)
        descriptors.extend(struct.pack("<128sQQ32s", name.encode(), offset,
                                       len(content), digest))
        payload.extend(content)
    descriptors.extend(bytes(176 * (MAX_FILES - len(entries))))
    total_bytes = HEADER_BYTES + len(payload)
    bundle = (struct.pack("<QQQQ", MAGIC, HEADER_BYTES, len(entries), total_bytes) +
              descriptors + payload)
    if len(bundle) != total_bytes:
        raise AssertionError("firmware bundle layout mismatch")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bundle)
    digest = hashlib.sha256(bundle).hexdigest()
    args.output.with_suffix(args.output.suffix + ".sha256").write_text(
        f"{digest}  {args.output.name}\n")
    print(f"firmware bundle files={len(entries)} bytes={len(bundle)} sha256={digest}")


if __name__ == "__main__":
    main()
