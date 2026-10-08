#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise offline boot staging without QEMU or writable backing disks."""

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import qemu_filed_cutover_smoke as cutover


class PrivateKernelStageTest(unittest.TestCase):
    def test_sandbox_only_never_sends_client_workload(self):
        with tempfile.TemporaryDirectory(prefix='sandbox-publication-') as temporary:
            root = Path(temporary)
            (root / 'console.sock').touch()
            for status in (0, -5):
                guest = Mock()
                guest.text = f'[Thread 7] FS_INTERNAL_BENCH_DONE status={status}\n'.encode()
                args = SimpleNamespace(out=root, timeout=10, native_smoke=False,
                                       sandbox_bench_only=True)
                with patch.object(cutover, 'overlay'), patch.object(cutover, 'qemu_common', return_value=[]), \
                        patch.object(cutover, 'CutoverGuest', return_value=guest):
                    if status:
                        with self.assertRaises(RuntimeError):
                            cutover.smoke(args, root / 'disk', root / 'boot')
                    else:
                        cutover.smoke(args, root / 'disk', root / 'boot')
                guest.send.assert_not_called()
                self.assertEqual(guest.wait.call_count, 1)
                guest.close.assert_called_once()
                self.assertEqual(json.loads((root / 'gates.json').read_text()),
                                 {'sandbox_benchmark_quiesced': status == 0})

    def test_explicit_kernel_is_frozen_into_private_fat_only(self):
        with tempfile.TemporaryDirectory(prefix="filed-boot-stage-") as temporary:
            root = Path(temporary)
            sources = {}
            for name in ("iso", "backing", "authority", "boot", "kernel", "init",
                         "storage_boot", "seed0root", "filed"):
                sources[name] = root / name
                sources[name].write_bytes((name + "-immutable").encode())
            package = root / "package"
            package.mkdir()
            (package / "manifest.bin").write_bytes(b"sealed-manifest")
            out = root / "run"
            commands = []

            def fake_run(command, cwd=cutover.ROOT):
                command = [str(value) for value in command]
                commands.append(command)
                if command[:2] == ["go", "run"]:
                    Path(command[command.index("-output") + 1]).write_bytes(b"archive")

            argv = ["cutover", "--stage-only", "--out", str(out),
                    "--iso", str(sources["iso"]), "--backing", str(sources["backing"]),
                    "--authority-disk", str(sources["authority"]),
                    "--boot-image", str(sources["boot"]), "--kernel", str(sources["kernel"]),
                    "--init-app", str(sources["init"]),
                    "--storage-boot", str(sources["storage_boot"]),
                    "--seed0root", str(sources["seed0root"]),
                    "--filed", str(sources["filed"]), "--package", str(package)]
            before = {name: cutover.digest(path) for name, path in sources.items()}
            with patch.object(sys, "argv", argv), patch.object(cutover, "run", fake_run), \
                    contextlib.redirect_stdout(io.StringIO()):
                cutover.main()
            metadata = json.loads((out / "inputs.json").read_text())
            self.assertEqual((out / "KERNEL.ELF").read_bytes(), b"kernel-immutable")
            self.assertEqual(metadata["kernel"], {
                "source": str(sources["kernel"]), "sha256": before["kernel"]})
            self.assertEqual(metadata["boot_template_sha256"], before["boot"])
            self.assertEqual(metadata["boot_template"], str(sources["boot"]))
            kernel_copy = [command for command in commands if command[-1] == "::/KERNEL.ELF"]
            self.assertEqual(kernel_copy, [["mcopy", "-o", "-i",
                str(out / "limine-boot.img") + "@@2097152",
                str(out / "KERNEL.ELF"), "::/KERNEL.ELF"]])
            self.assertEqual(before, {name: cutover.digest(path) for name, path in sources.items()})
            self.assertFalse((out / "prepared.qcow2").exists())


if __name__ == "__main__":
    unittest.main()
