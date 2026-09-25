import importlib.util
from pathlib import Path
import tempfile
import unittest
import runpy

REPO = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("stage_void_webkit", REPO / "tools/stage_void_webkit.py")
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)


class OverlayTests(unittest.TestCase):
    def test_selected_libraries_activation_and_repeat(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime, webkit, icu, epiphany = (root / name for name in ("runtime", "webkit", "icu", "epiphany"))

            def put(base, relative, data):
                path = base / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                return path

            for name in ("libwebkitgtk-6.0.so.4.13.8", "libjavascriptcoregtk-6.0.so.1.6.10"):
                put(webkit, "usr/lib/" + name, b"new engine")
                (webkit / "usr/lib" / name.rsplit(".", 2)[0]).symlink_to(name)
            put(webkit, "usr/libexec/webkitgtk-6.0/WebKitWebProcess", b"new process")
            put(webkit, "usr/lib/webkitgtk-6.0/injected-bundle/bundle.so", b"new bundle")
            put(webkit, "usr/lib/girepository-1.0/WebKit-6.0.typelib", b"new metadata")
            put(epiphany, "usr/bin/epiphany", b"new browser")
            put(epiphany, "usr/lib/epiphany/libephymain.so", b"browser private library")
            put(epiphany, "usr/lib/epiphany/web-process-extensions/libephywebprocessextension.so", b"browser extension")
            put(icu, "usr/lib/libicuuc.so.78.3", b"new ICU")
            put(icu, "usr/share/icu/78.3/icudt78l.dat", b"ICU runtime data")
            (icu / "usr/lib/libicuuc.so.78").symlink_to("libicuuc.so.78.3")
            put(icu, "usr/lib/unrelated.so", b"must not be copied")
            old = put(runtime, "usr/lib/libwebkitgtk-6.0.so.4.11.6", b"old engine")
            public = runtime / "usr/lib/libwebkitgtk-6.0.so.4"
            public.symlink_to(old.name)
            font = put(runtime, "usr/share/fonts/existing.ttf", b"font")
            mesa = put(runtime, "usr/lib/libgallium-25.1.9.so", b"mesa")
            old_icu_data = put(runtime, "usr/share/icu/76.1/icudt76l.dat", b"old ICU data")
            service = put(runtime, "usr/share/dbus-1/services/org.gnome.Epiphany.service",
                          b"[D-BUS Service]\nExec=/usr/bin/epiphany --gapplication-service\n")
            desktop = put(runtime, "usr/share/applications/org.gnome.Epiphany.desktop",
                          b"[Desktop Entry]\nExec=epiphany %U\n")
            for _ in range(2):
                overlay.stage(runtime, [("libwebkitgtk60-test", webkit), ("libicu78-test", icu), ("epiphany-test", epiphany)])
            self.assertEqual(public.read_bytes(), b"new engine")
            self.assertEqual(old.read_bytes(), b"old engine")
            self.assertEqual(font.read_bytes(), b"font")
            self.assertEqual(mesa.read_bytes(), b"mesa")
            self.assertEqual(old_icu_data.read_bytes(), b"old ICU data")
            self.assertEqual((runtime / "usr/share/icu/78.3/icudt78l.dat").read_bytes(),
                             b"ICU runtime data")
            self.assertEqual((runtime / "usr/lib/libicuuc.so.78").read_bytes(), b"new ICU")
            self.assertFalse((runtime / "usr/lib/unrelated.so").exists())
            self.assertEqual((runtime / "usr/lib64/webkitgtk-6.0/injected-bundle/bundle.so").read_bytes(), b"new bundle")
            self.assertEqual((runtime / "usr/bin/epiphany").read_bytes(), b"new browser")
            self.assertEqual((runtime / "usr/lib64/epiphany/libephymain.so").read_bytes(), b"browser private library")
            self.assertEqual((runtime / "usr/lib64/epiphany/web-process-extensions/libephywebprocessextension.so").read_bytes(), b"browser extension")
            self.assertIn("Exec=/usr/local/bin/epiphany --gapplication-service", service.read_text())
            self.assertIn("Exec=/usr/local/bin/epiphany %U", desktop.read_text())
            self.assertIn("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1",
                          (runtime / "usr/local/bin/epiphany").read_text())
            # pacgo manifests consume regular marker files, not host symlinks.
            rootfs = runpy.run_path(str(REPO / "tools/rootfs_overlay.py"))
            rootfs["deduplicate"](runtime, [])
            for name in ("epiphany", "webkitgtk-6.0"):
                alias = runtime / "usr/lib64" / name
                self.assertFalse(alias.is_symlink())
                self.assertEqual(alias.read_bytes(), rootfs["MARKER"] + ("../lib/" + name).encode())

    def test_reject_external_library_link(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            link = root / "external.so"
            link.symlink_to("/etc/passwd")
            with self.assertRaises(ValueError):
                overlay.copy_entry(link, root / "output.so")

    def test_reject_missing_icu_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "icu/usr/lib").mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "runtime data missing"):
                overlay.stage(root / "runtime", [("libicu78-test", root / "icu")])


if __name__ == "__main__":
    unittest.main()
