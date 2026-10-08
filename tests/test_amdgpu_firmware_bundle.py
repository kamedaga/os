#!/usr/bin/env python3
"""Exercise the owned bundle producer/parser with synthetic firmware bytes."""

import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FirmwareBundleTests(unittest.TestCase):
    def test_real_producer_and_parser_unversioned_extent(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".artifacts") as directory:
            work = Path(directory)
            firmware = work / "firmware"
            (firmware / "amdgpu").mkdir(parents=True)
            manifest = work / "firmware.sha256"
            entries = []
            for index in range(15):
                name = f"amdgpu/synthetic-{index}.bin"
                content = f"synthetic firmware {index}".encode()
                (firmware / name).write_bytes(content)
                entries.append(f"{hashlib.sha256(content).hexdigest()}  {name}\n")
            manifest.write_text("".join(entries))
            bundle = work / "firmware.bundle"
            subprocess.run([
                sys.executable, str(ROOT / "pack/scripts/build_amdgpu_firmware_bundle.py"),
                "--manifest", str(manifest), "--firmware-dir", str(firmware),
                "--output", str(bundle),
            ], check=True, capture_output=True, text=True)
            data = bundle.read_bytes()
            magic, header_bytes, count, total = struct.unpack_from("<QQQQ", data)
            self.assertEqual(magic, 0x314C444E424D5746)
            self.assertEqual(header_bytes, 32 + 176 * 64)
            self.assertEqual(count, 15)
            self.assertEqual(total, len(data))
            self.assertEqual(bundle.with_suffix(".bundle.sha256").read_text().split()[0],
                             hashlib.sha256(data).hexdigest())
            checker = work / "bundle-test"
            subprocess.run([
                "/usr/bin/cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-g", "-fsanitize=address,undefined",
                "-I" + str(ROOT / "userland/kobox2_adapter"),
                "-I" + str(ROOT / "kobox2/linux-sandbox/kobox"),
                "-I" + str(ROOT / "kobox2/protocol/include"),
                str(ROOT / "tests/amdgpu_firmware_bundle_test.c"),
                str(ROOT / "userland/kobox2_adapter/firmware_bundle.c"),
                str(ROOT / "kobox2/protocol/src/sha256.c"), "-o", str(checker),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(checker), str(bundle)], check=True,
                                    capture_output=True, text=True)
            self.assertIn("extent legacy-rejection", result.stdout)


if __name__ == "__main__":
    unittest.main()
