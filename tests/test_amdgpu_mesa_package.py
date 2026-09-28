#!/usr/bin/env python3
"""Dependency and integrity checks for the optional AMDGPU Mesa payload."""

import importlib.util
import json
from pathlib import Path
import tempfile
import tarfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "pack/scripts/build_amdgpu_mesa_package.py"
SPEC = importlib.util.spec_from_file_location("amdgpu_mesa_package", SCRIPT)
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class PackageTests(unittest.TestCase):
    def test_marker_and_link_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "alias.so"
            marker.write_bytes(PACKAGE.MARKER + b"real.so")
            self.assertEqual(PACKAGE.marker_target(marker), "real.so")
            self.assertEqual(PACKAGE.link_destination("usr/lib/alias.so", "real.so"),
                             "usr/lib/real.so")
            with self.assertRaises(ValueError):
                PACKAGE.link_destination("usr/lib/alias.so", "../../../outside")

    def test_closure_rejects_missing_elf_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / "mesa/usr/lib/start.so"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"\x7fELFsynthetic")
            closure = PACKAGE.Closure(root / "mesa", root / "clang")
            with patch.object(PACKAGE, "elf_needed", return_value=["absent.so"]):
                with self.assertRaisesRegex(FileNotFoundError, "absent.so"):
                    closure.add("usr/lib/start.so")

    def test_closure_rejects_marker_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "mesa/usr/lib"
            root.mkdir(parents=True)
            (root / "a.so").write_bytes(PACKAGE.MARKER + b"b.so")
            (root / "b.so").write_bytes(PACKAGE.MARKER + b"a.so")
            closure = PACKAGE.Closure(Path(directory) / "mesa", Path(directory) / "clang")
            with self.assertRaisesRegex(ValueError, "cycle"):
                closure.add("usr/lib/a.so")

    def test_manifest_rejects_modified_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            payload = output / "rootfs/usr/lib/example.so"
            payload.parent.mkdir(parents=True)
            payload.write_bytes(b"original")
            manifest = {
                "schema": 1, "profile": "amdgpu", "mesa_version": PACKAGE.VERSION,
                "external": sorted(PACKAGE.EXTERNAL), "total_bytes": len(b"original"),
                "entries": [{"path": "/usr/lib/example.so", "bytes": len(b"original"),
                             "sha256": PACKAGE.digest(payload)}],
            }
            (output / "manifest.json").write_text(json.dumps(manifest))
            PACKAGE.verify(output)
            payload.write_bytes(b"modified")
            with self.assertRaisesRegex(ValueError, "digest mismatch"):
                PACKAGE.verify(output)

    def test_archive_materializes_checked_link_without_copying_rootfs(self):
        with tempfile.TemporaryDirectory(dir=PACKAGE.REPO / ".artifacts") as directory:
            package = Path(directory) / "package"
            root = package / "rootfs/usr/lib"
            root.mkdir(parents=True)
            (root / "real.so").write_bytes(b"payload")
            (root / "alias.so").write_bytes(PACKAGE.MARKER + b"real.so")
            entries = []
            for name in ("alias.so", "real.so"):
                payload = root / name
                entries.append({"path": f"/usr/lib/{name}",
                                "bytes": payload.stat().st_size,
                                "sha256": PACKAGE.digest(payload)})
            (package / "manifest.json").write_text(json.dumps({
                "schema": 1, "profile": "amdgpu", "mesa_version": PACKAGE.VERSION,
                "external": sorted(PACKAGE.EXTERNAL),
                "total_bytes": sum(entry["bytes"] for entry in entries),
                "entries": entries,
            }))
            archive_path = Path(directory) / "runtime.tar.gz"
            first_digest = PACKAGE.build_archive(package, archive_path)
            self.assertEqual(first_digest, PACKAGE.digest(archive_path))
            self.assertEqual(first_digest, PACKAGE.build_archive(package, archive_path))
            self.assertEqual(archive_path.read_bytes()[4:8], b"\0\0\0\0")
            with tarfile.open(archive_path, "r:gz") as archive:
                alias = archive.getmember("usr/lib/alias.so")
                self.assertTrue(alias.issym())
                self.assertEqual(alias.linkname, "real.so")
                self.assertEqual(archive.extractfile("usr/lib/real.so").read(),
                                 b"payload")


if __name__ == "__main__":
    unittest.main()
