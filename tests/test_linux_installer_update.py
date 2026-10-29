#!/usr/bin/env python3
"""Check an incremental ISO/USB update using a small disposable payload."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("xorriso"), "xorriso unavailable")
class IncrementalUpdate(unittest.TestCase):
    def test_patch_reproduces_new_iso_without_writing_rootfs(self):
        with tempfile.TemporaryDirectory(prefix="installer-update-", dir=ROOT/".artifacts") as directory:
            work = Path(directory)
            tree = work/"tree/pacha"
            tree.mkdir(parents=True)
            payload = bytes(range(256))*4096
            (tree/"ROOTFS.TAR").write_bytes(payload)
            image = work/"test.iso"
            subprocess.run(["xorriso", "-outdev", str(image), "-map", str(tree.parent), "/"],
                           capture_output=True, check=True)
            original = image.read_bytes()
            command = ["python3", str(ROOT/"pack/scripts/update_linux_installer.py"),
                       "--image", str(image), "--expect-sha256", "0"*64,
                       "--output-dir", str(work/"patch")]
            wrong = subprocess.run(command, capture_output=True)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertEqual(original, image.read_bytes())
            command[command.index("0"*64)] = hashlib.sha256(original).hexdigest()
            subprocess.run(command, capture_output=True, check=True)
            manifest = json.loads((work/"patch/usb-patch.json").read_text())
            patch = (work/"patch/usb-patch.bin").read_bytes()
            self.assertEqual(hashlib.sha256(patch).hexdigest(), manifest["patch_sha256"])
            actual = bytearray(original)
            actual.extend(bytes(manifest["after_bytes"]-len(actual)))
            protected = manifest["rootfs_extent"]
            for region in manifest["ranges"]:
                start, size, offset = region["offset"], region["bytes"], region["patch_offset"]
                self.assertFalse(start < protected["offset"]+protected["bytes"] and
                                 start+size > protected["offset"])
                actual[start:start+size] = patch[offset:offset+size]
            self.assertEqual(actual, image.read_bytes())
            self.assertEqual(hashlib.sha256(actual).hexdigest(), manifest["after_sha256"])
            self.assertEqual(actual[protected["offset"]:protected["offset"]+len(payload)], payload)


if __name__ == "__main__": unittest.main()
