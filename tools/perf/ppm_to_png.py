#!/usr/bin/env python3
"""Convert the binary PPM emitted by x11_probe to a dependency-free PNG."""
import argparse
import struct
import zlib
from pathlib import Path


def chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    with args.source.open("rb") as source:
        assert source.readline() == b"P6\n"
        width, height = map(int, source.readline().split())
        assert source.readline() == b"255\n"
        pixels = source.read()
    assert len(pixels) == width * height * 3
    scanlines = b"".join(b"\0" + pixels[row * width * 3:(row + 1) * width * 3]
                         for row in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(scanlines, 6))
           + chunk(b"IEND", b""))
    args.destination.write_bytes(png)


if __name__ == "__main__":
    main()
