#!/usr/bin/env python3
"""Verify the emitted desktop, its Mesa identity and target ELF dependencies."""
import hashlib
import argparse
import importlib.util
import json
import os
from pathlib import Path
import posixpath
import re
import subprocess
import tarfile
import tempfile
import configparser
import shlex
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "pack/scripts/build_live_desktop_bundle.py"
spec = importlib.util.spec_from_file_location("desktop", SCRIPT)
desktop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bootfs-manifest", type=Path,
                        help="Audit the actual combined bootfs/desktop libraries and RAM footprint")
    args = parser.parse_args()
    host = {}
    if args.bootfs_manifest:
        for line in args.bootfs_manifest.read_text().splitlines():
            if line and not line.startswith("#"):
                target, source = line.split("=", 1)
                host[target.lstrip("/")] = (args.bootfs_manifest.parent / source).resolve()
    bundle = ROOT / ".artifacts/live-desktop/desktop.tar.gz"
    manifest = json.loads(bundle.with_suffix(bundle.suffix + ".json").read_text())
    assert desktop.sha256(bundle) == manifest["sha256"]
    mesa, mesa_manifest = desktop.mesa_package(desktop.DEFAULT_MESA)
    with tarfile.open(bundle, "r:gz") as archive:
        members = {item.name: item for item in archive.getmembers()}
        assert len(members) == len(archive.getmembers())
        for name in members:
            desktop.checked_relative(name)
        assert not (desktop.HOST_OWNED & members.keys())
        inventory = archive.extractfile("usr/share/pacha/live-desktop-files.sha256").read().decode()
        links = archive.extractfile("usr/share/pacha/live-desktop-links").read().decode()
        external = set(host) if args.bootfs_manifest else set(desktop.HOST_OWNED)
        # Dynamic resource lookups are not described by ELF DT_NEEDED. A
        # complete shared-library closure alone missed GTK's SVG loader and
        # default icon theme, leaving an otherwise running desktop blank.
        cache = archive.extractfile("usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache").read().decode()
        for loader in re.findall(r'^"(/[^"\n]+\.so)"$', cache, re.MULTILINE):
            assert loader.lstrip("/") in members, f"pixbuf cache references missing loader: {loader}"
        assert "usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader_svg.so" in members
        assert "usr/share/icons/Adwaita/index.theme" in members
        # Generated indexes are not APK-owned files. They must accompany
        # every selected theme, including hicolor's application icons.
        for name in members:
            if name.startswith('usr/share/icons/') and name.endswith('/index.theme'):
                cache_name = name.removesuffix('/index.theme') + '/icon-theme.cache'
                assert cache_name in members, f'icon cache missing for selected theme: {name}'
                data = archive.extractfile(cache_name).read()
                assert len(data) >= 12 and data[:4] == b'\x00\x01\x00\x00', cache_name
                theme = name.removesuffix('/index.theme')
                assert members[theme].isdir()
                assert members[theme].mtime <= members[cache_name].mtime
                assert members[theme].offset > members[cache_name].offset

        def resolved(name, seen=frozenset()):
            assert name not in seen, f"link cycle: {name}"
            item = members.get(name)
            if item and item.issym():
                target = mesa.link_destination(name, item.linkname)
                return resolved(target, seen | {name})
            return name

        # Follow the shipped panel defaults rather than just checking .so
        # dependencies: Garcon silently filters launchers with missing Exec.
        panel = ET.fromstring(archive.extractfile("etc/xdg/xfce4/panel/default.xml").read())
        launchers = panel.findall("./property[@name='plugins']/property[@value='launcher']")
        assert launchers
        for launcher in launchers:
            for item in launcher.findall("./property[@name='items']/value"):
                name = "usr/share/applications/" + item.attrib["value"]
                assert name in members, f"panel launcher missing desktop entry: {name}"
                entry = configparser.ConfigParser(interpolation=None, strict=False)
                entry.read_string(archive.extractfile(name).read().decode())
                for key in ("Exec", "TryExec"):
                    command = entry["Desktop Entry"].get(key)
                    if not command:
                        continue
                    executable = shlex.split(command)[0]
                    candidates = [executable.lstrip("/")] if executable.startswith("/") else [
                        f"{directory}/{executable}" for directory in ("usr/bin", "bin", "usr/sbin", "sbin")]
                    assert any(resolved(path) in members or resolved(path) in external
                               for path in candidates), f"{name}: missing launcher {key}={executable}"

        for line in links.splitlines():
            name, target = line.split("\t")
            assert members[name].issym() and members[name].linkname == target
            target_path = resolved(name)
            assert target_path in members or target_path in external or any(
                path.startswith(target_path + "/") for path in members.keys() | external
            ), f"missing link target: {name}"
        for entry in mesa_manifest["entries"]:
            name = entry["path"].lstrip("/")
            if name in external:
                continue
            path = desktop.DEFAULT_MESA / "rootfs" / name
            target = mesa.marker_target(path)
            if target is not None:
                assert members[name].issym() and members[name].linkname == target
            else:
                assert hashlib.sha256(archive.extractfile(name).read()).hexdigest() == entry["sha256"]
        with tempfile.TemporaryDirectory(dir=ROOT / ".artifacts") as tmp:
            elf_path = Path(tmp) / "elf"
            elf_count = 0
            objects = {}

            def inspect(name, content):
                elf_path.write_bytes(content)
                dynamic = subprocess.run(["readelf", "-d", elf_path], check=True,
                                         capture_output=True, text=True).stdout
                symbols = subprocess.run(["readelf", "--dyn-syms", "--wide", elf_path],
                                         check=True, capture_output=True, text=True).stdout
                exports, undefined = set(), set()
                for line in symbols.splitlines():
                    fields = line.split()
                    if len(fields) < 8 or not fields[0].endswith(":"):
                        continue
                    symbol = fields[7].split("@")[0]
                    if fields[6] != "UND" and fields[4] in ("GLOBAL", "WEAK") and fields[5] in ("DEFAULT", "PROTECTED"):
                        exports.add(symbol)
                    elif fields[6] == "UND" and fields[4] == "GLOBAL":
                        undefined.add(symbol)
                objects[name] = (re.findall(r"\(NEEDED\).*\[([^]]+)\]", dynamic), exports, undefined)
                return objects[name][0]

            for line in inventory.splitlines():
                expected, name = line.split("  ", 1)
                content = archive.extractfile(name).read()
                assert hashlib.sha256(content).hexdigest() == expected, name
                if content[:4] != b"\x7fELF":
                    continue
                elf_count += 1
                for soname in inspect(name, content):
                    candidates = (f"usr/lib/{soname}", f"lib/{soname}")
                    assert any(resolved(path) in members or resolved(path) in external
                               for path in candidates), f"{name}: missing {soname}"
            if args.bootfs_manifest:
                for name, source in host.items():
                    if name in members:
                        continue
                    content = source.read_bytes()
                    if content[:4] == b"\x7fELF":
                        inspect(name, content)

                def dependency(soname):
                    for directory in ("lib", "usr/lib"):
                        candidate = resolved(f"{directory}/{soname}")
                        if candidate in objects:
                            return candidate
                    raise AssertionError(f"combined runtime missing {soname}")

                for executable in ("usr/libexec/Xorg", "usr/bin/xinit", "usr/bin/xfce4-session",
                                   "usr/bin/xfwm4", "usr/bin/xfdesktop", "usr/bin/xfce4-panel",
                                   "usr/bin/gtk3-demo", "usr/bin/dbus-daemon",
                                   "usr/bin/gdk-pixbuf-query-loaders", "usr/bin/exo-open", "usr/bin/xfce4-appfinder",
                                   "usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader_svg.so"):
                    pending = [executable, "lib/ld-musl-x86_64.so.1"]
                    scope = set()
                    while pending:
                        name = resolved(pending.pop())
                        if name in scope:
                            continue
                        scope.add(name)
                        pending.extend(dependency(soname) for soname in objects[name][0])
                    exports = set().union(*(objects[name][1] for name in scope))
                    for name in scope:
                        assert not (objects[name][2] - exports), \
                            f"{executable}: {name}: unresolved {sorted(objects[name][2] - exports)}"

                # Count the combined namespace, not just the compressed
                # archive. Import runs beside resident boot services; checking
                # their footprints separately missed the old 512 MiB ceiling.
                sizes = {name: source.stat().st_size for name, source in host.items()}
                sizes.update({name: item.size if item.isfile() else len(item.linkname)
                              for name, item in members.items()})
                required_pages = sum((size + 4095) // 4096 for size in sizes.values())
                header = (ROOT / "userland/filed/include/filed/tmpfs_internal.h").read_text()
                capacity = int(re.search(r"FILED_TMPFS_PAGE_POOL_PAGES\s*=\s*(\d+)", header)[1])
                assert required_pages < capacity, \
                    f"combined runtime needs {required_pages} pages, tmpfs permits {capacity}"
                namespace = set(sizes)
                for name in sizes:
                    namespace.update(str(parent) for parent in Path(name).parents if str(parent) != ".")
                inode_capacity = int(re.search(r"FILED_TMPFS_MAX_INODES\s*=\s*(\d+)", header)[1])
                assert len(namespace) + 1 + 1024 < inode_capacity, \
                    f"combined runtime leaves insufficient session inode headroom: {len(namespace) + 1}/{inode_capacity}"
                print(f"PASS: combined symbols and RAM footprint {required_pages * 4096 / 1048576:.1f} MiB "
                      f"within {capacity * 4096 / 1048576:.0f} MiB demand-allocated budget")
        required = ("usr/libexec/Xorg", "usr/bin/startx", "usr/bin/startxfce4",
                    "usr/bin/gtk3-demo", "usr/bin/dbus-uuidgen", "sbin/apk",
                    "usr/lib/dri/radeonsi_dri.so")
        for name in required:
            assert name in members
        print(f"PASS: {manifest['package_count']} packages, {elf_count} ELF closures, "
              f"{len(manifest['mesa_overrides'])} authoritative Mesa entries")


if __name__ == "__main__":
    main()
