#!/usr/bin/env python3

from __future__ import annotations

import hashlib
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
import alpine_packages


class AlpinePackagesTest(unittest.TestCase):
    def test_materialize_uses_verified_cache_without_network(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            cache = root / "cache"
            cache.mkdir()
            payload = root / "payload"
            (payload / "usr/bin").mkdir(parents=True)
            (payload / "usr/bin/example").write_text("example\n")
            apk = cache / "example-1.0-r0.apk"
            with tarfile.open(apk, "w:gz") as archive:
                archive.add(payload / "usr", arcname="usr")
            sha256 = hashlib.sha256(apk.read_bytes()).hexdigest()
            lock = root / "packages.lock"
            lock.write_text(f"main example 1.0-r0 {sha256}\n")
            output = root / "output"
            args = type(
                "Args",
                (),
                {
                    "lock": lock,
                    "package": [],
                    "cache": cache,
                    "output": output,
                    "mirror": "https://invalid.example",
                    "branch": "v3.22",
                    "arch": "x86_64",
                },
            )()
            alpine_packages.materialize(args)
            self.assertEqual((output / "usr/bin/example").read_text(), "example\n")

    def test_lock_rejects_duplicate_package(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            lock = Path(raw) / "packages.lock"
            digest = "0" * 64
            lock.write_text(
                f"main duplicate 1-r0 {digest}\n"
                f"community duplicate 2-r0 {digest}\n"
            )
            with self.assertRaises(SystemExit):
                alpine_packages.read_lock(lock)


if __name__ == "__main__":
    unittest.main()
