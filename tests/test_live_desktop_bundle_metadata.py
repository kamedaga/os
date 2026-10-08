#!/usr/bin/env python3
"""Check actual metadata emission using a tiny synthetic package inventory."""

import importlib.util
import io
import json
from pathlib import Path
import tempfile
import tarfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "desktop_bundle", ROOT / "pack/scripts/build_live_desktop_bundle.py")
BUNDLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUNDLE)


class DesktopMetadataTests(unittest.TestCase):
    def test_actual_archive_preserves_named_owner_with_local_and_apk_payloads(self):
        with tempfile.TemporaryDirectory(dir=ROOT / '.artifacts') as directory:
            work = Path(directory)
            source = work / 'source'
            (source / 'lib/apk/db').mkdir(parents=True)
            name = 'usr/share/polkit-1/rules.d/50-default.rules'
            (source / 'lib/apk/db/installed').write_text(
                'P:synthetic\nV:1\nF:usr/share/polkit-1/rules.d\nR:50-default.rules\n\n')
            (source / 'etc/apk/keys').mkdir(parents=True)
            (source / 'etc/apk/keys/test.rsa.pub').write_bytes(b'key')
            cache = work / 'cache'; cache.mkdir()
            apk = cache / 'synthetic-1.apk'
            with tarfile.open(apk, 'w:gz') as archive:
                entry = tarfile.TarInfo(name)
                entry.mode = 0o700
                entry.uname = entry.gname = 'polkitd'
                entry.size = 3
                archive.addfile(entry, io.BytesIO(b'apk'))
            api_spec = importlib.util.spec_from_file_location(
                'mesa_owner_test', ROOT / 'pack/scripts/build_amdgpu_mesa_package.py')
            api = importlib.util.module_from_spec(api_spec); api_spec.loader.exec_module(api)
            mesa = work / 'mesa'; mesa.mkdir()
            (mesa / 'manifest.json').write_text(json.dumps({
                'profile': 'amdgpu', 'mesa_version': api.VERSION,
                'external': sorted(api.EXTERNAL), 'entries': [], 'total_bytes': 0}))
            local = source / name; local.parent.mkdir(parents=True)
            local.write_bytes(b'local'); local.chmod(0o600)
            with patch.object(BUNDLE, 'SEEDS', ('synthetic',)), patch.object(BUNDLE, 'EXTRA', ()), \
                 patch.object(BUNDLE, 'PACKAGE_CACHE', cache), \
                 patch.object(BUNDLE, 'locked_versions', return_value={'synthetic': ('1', BUNDLE.sha256(apk))}):
                for use_local in (True, False):
                    if not use_local: local.unlink()
                    output = work / ('local.tar.gz' if use_local else 'fallback.tar.gz')
                    BUNDLE.build(source, output, mesa)
                    with tarfile.open(output, 'r:gz') as archive:
                        emitted = archive.getmember(name)
                        self.assertEqual((emitted.uid, emitted.gid, emitted.mode), (102, 102, 0o700))
                        self.assertEqual(archive.extractfile(emitted).read(), b'local' if use_local else b'apk')

    def test_package_named_owner_is_not_root_or_host_uid(self):
        packaged = tarfile.TarInfo('usr/share/polkit-1/rules.d/50-default.rules')
        packaged.uid = packaged.gid = 0
        packaged.uname = packaged.gname = 'polkitd'
        packaged.mode = 0o700
        for local_uid in (0, 1000, 65534):
            emitted = tarfile.TarInfo(packaged.name)
            emitted.uid = emitted.gid = local_uid
            emitted.mode = 0o777
            BUNDLE.preserve_package_metadata(emitted, packaged, BUNDLE.target_account_ids())
            self.assertEqual((emitted.uid, emitted.gid, emitted.mode), (102, 102, 0o700))
            self.assertEqual((emitted.uname, emitted.gname, emitted.mtime), ('', '', 0))
        # Missing local files take the APK fallback branch with the same item
        # as input and output, not a second metadata policy.
        BUNDLE.preserve_package_metadata(packaged, packaged, BUNDLE.target_account_ids())
        self.assertEqual((packaged.uid, packaged.gid, packaged.mode), (102, 102, 0o700))

    def test_unknown_package_owner_fails_closed(self):
        packaged = tarfile.TarInfo('private.rules')
        packaged.uname = 'not-a-target-account'
        with self.assertRaisesRegex(ValueError, 'absent from target base'):
            BUNDLE.preserve_package_metadata(tarfile.TarInfo(packaged.name), packaged,
                                             BUNDLE.target_account_ids())

    def test_actual_writer_uses_content_metadata_without_schema_counter(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".artifacts") as directory:
            work = Path(directory)
            source = work / "source"
            (source / "lib/apk/db").mkdir(parents=True)
            (source / "lib/apk/db/installed").write_bytes(b"")
            (source / "etc/apk/keys").mkdir(parents=True)
            (source / "etc/apk/keys/synthetic.rsa.pub").write_bytes(b"synthetic key")
            mesa = work / "mesa"
            mesa.mkdir()
            # No external binary or rootfs is materialized. Empty inputs keep
            # this test focused on the archive/sidecar writer, not Xfce closure.
            api_spec = importlib.util.spec_from_file_location(
                "mesa_metadata", ROOT / "pack/scripts/build_amdgpu_mesa_package.py")
            api = importlib.util.module_from_spec(api_spec)
            api_spec.loader.exec_module(api)
            (mesa / "manifest.json").write_text(json.dumps({
                "profile": "amdgpu", "mesa_version": api.VERSION,
                "external": sorted(api.EXTERNAL), "entries": [], "total_bytes": 0,
            }))
            output = work / "synthetic.tar.gz"
            with patch.object(BUNDLE, "SEEDS", ()), patch.object(BUNDLE, "EXTRA", ()):
                manifest = BUNDLE.build(source, output, mesa)
            actual = json.loads(output.with_suffix(".gz.json").read_text())
            self.assertEqual(actual, manifest)
            self.assertNotIn("schema", actual)
            self.assertEqual(actual["source"], "Alpine v3.22 locked Xfce packages")
            self.assertEqual(actual["archive_bytes"], output.stat().st_size)
            self.assertEqual(actual["sha256"], BUNDLE.sha256(output))


if __name__ == "__main__":
    unittest.main()
