#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check native/Linux runtime packaging without building or staging a rootfs."""
import importlib.util
from pathlib import Path
import shlex
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "desktop_bundle", ROOT / "pack/scripts/build_live_desktop_bundle.py")
DESKTOP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DESKTOP)


class RuntimeNamespaces(unittest.TestCase):
    def test_installer_native_paths_do_not_replace_linux_loader(self):
        installs = {}
        for line in (ROOT / "pack/scripts/build_installer_rootfs.sh").read_text().splitlines():
            if not line.startswith("install_runtime_artifact "):
                continue
            fields = shlex.split(line, comments=True)
            if fields and fields[0] == "install_runtime_artifact":
                self.assertNotIn(fields[3], installs)
                installs[fields[3]] = (fields[1], fields[2])
        self.assertEqual(installs["/usr/lib/pachaos/ld-musl-x86_64.so.1"],
                         ("pachaos_musl_ldso", "PAMUSLLD.SO"))
        self.assertEqual(installs["/usr/lib/pachaos/libc.so"],
                         ("pachaos_musl_libc", "PAMUSLLC.SO"))
        # The fresh Alpine base supplies these paths. Installing native bytes
        # there would corrupt Linux PT_INTERP consumers and symlink targets.
        self.assertNotIn("/lib/ld-musl-x86_64.so.1", installs)
        self.assertNotIn("/lib/libc.so", installs)
        for path in ("/lib/linux/ld-musl-x86_64.so.1", "/lib/linux/libc.so",
                     "/lib/libc.musl-x86_64.so.1"):
            self.assertEqual(installs[path][0], "lpr_linux_musl_libc")

    def test_live_runtime_mappings_are_disjoint(self):
        mappings = {}
        for line in (ROOT / "pack/live-bootfs.yaml").read_text().splitlines():
            if line.startswith("  /"):
                path, app = line.strip().split(": ", 1)
                mappings[path] = app
        expected = {
            "/usr/lib/pachaos/ld-musl-x86_64.so.1": "pachaos_musl_ldso",
            "/usr/lib/pachaos/libc.so": "pachaos_musl_libc",
            "/lib/ld-musl-x86_64.so.1": "lpr_linux_musl_libc",
            "/lib/libc.so": "lpr_linux_musl_libc",
        }
        for path, app in expected.items():
            self.assertEqual(mappings[path], app)
            self.assertIn(path.lstrip("/"), DESKTOP.HOST_OWNED)
        for path, app in mappings.items():
            if app.startswith("pachaos_musl_"):
                self.assertTrue(path.startswith("/usr/lib/pachaos/"))


if __name__ == "__main__":
    unittest.main()
