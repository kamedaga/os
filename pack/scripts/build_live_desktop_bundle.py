#!/usr/bin/env python3
"""Select the locked Xfce runtime from its installed Alpine package records."""

from __future__ import annotations

import argparse
import importlib.util
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import os
import shutil
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = ROOT / ".artifacts/userland-fixtures/alpine-xfce-root"
DEFAULT_OUTPUT = ROOT / ".artifacts/live-desktop/desktop.tar.gz"
DEFAULT_MESA = ROOT / ".artifacts/kobox2-amdgpu-mesa-package"
LOCK = ROOT / "tools/manifests/alpine-xfce-v3.22-x86_64.lock"
PACKAGE_CACHE = ROOT / ".artifacts/third_party/alpine-xfce-v3.22-x86_64"
SEEDS = (
    "apk-tools", "xorg-server", "xinit", "xf86-input-libinput",
    "xfce4-session", "xfwm4", "xfdesktop", "xfce4-panel",
    "xfce4-settings", "xfce4-terminal", "gtk+3.0-demo",
    "xrandr", "font-dejavu", "dbus-x11",
    # The stock panel resolves launcher Exec commands at runtime. exo-libs
    # alone does not provide exo-open, and appfinder is not an ELF dependency
    # of the panel; omitting either empties its default launcher slots.
    "exo", "xfce4-appfinder",
    # These are runtime resources, not ELF DT_NEEDED dependencies. GTK's
    # standard icon fallback and the generated pixbuf cache require them.
    "adwaita-icon-theme", "librsvg",
)
HOST_OWNED = {
    "bin/ash", "bin/busybox", "bin/sh", "bin/curl",
    "etc/group", "etc/passwd", "etc/hosts", "etc/ssl/cert.pem",
    "etc/ssl/certs/ca-certificates.crt", "etc/ld-musl-x86_64.path",
    "lib/ld-musl-x86_64.so.1", "lib/libc.so", "lib/libc.musl-x86_64.so.1",
    "lib/linux/ld-musl-x86_64.so.1", "lib/linux/libc.so",
    # Desktop imports must not replace the native interpreter or its libc;
    # Linux applications retain their unmodified loader in the /lib namespace.
    "usr/lib/pachaos/ld-musl-x86_64.so.1", "usr/lib/pachaos/libc.so",
    "lib/libssl.so.3", "lib/libcrypto.so.3", "lib/libz.so.1",
    "lib/libzstd.so.1", "lib/libbrotlidec.so.1",
    "lib/libbrotlicommon.so.1", "lib/libunistring.so.5",
    "lib/libcares.so.2", "lib/libnghttp2.so.14", "lib/libidn2.so.0",
    "lib/libpsl.so.5", "usr/sbin/dropbear", "usr/bin/dropbearkey",
}
EXTRA = (
    "usr/share/glib-2.0/schemas/gschemas.compiled",
    "usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache",
    "usr/share/mime/mime.cache",
    "root/.xinitrc", "etc/apk/arch", "etc/apk/repositories",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def target_account_ids() -> tuple[dict[str, int], dict[str, int]]:
    # APK records often carry uid=0 with uname=polkitd. Host IDs and root
    # normalization both lose that authority; resolve against the base that
    # supplies HOST_OWNED passwd/group, not the builder's account database.
    base = ROOT / "userland/fixtures/base"
    def ids(filename: str) -> dict[str, int]:
        return {fields[0]: int(fields[2]) for line in (base / filename).read_text().splitlines()
                if line and not line.startswith('#') for fields in [line.split(':')]}
    return ids('passwd'), ids('group')


def preserve_package_metadata(info: tarfile.TarInfo, packaged: tarfile.TarInfo,
                              accounts: tuple[dict[str, int], dict[str, int]]) -> None:
    for field, name, table in (('uid', packaged.uname, accounts[0]),
                               ('gid', packaged.gname, accounts[1])):
        if name and name not in table:
            raise ValueError(f"package owner absent from target base: {name}: {info.name}")
        setattr(info, field, table[name] if name else getattr(packaged, field))
    info.mode = packaged.mode
    info.mtime = 0
    # Extraction may run outside the target chroot, whose same-named
    # accounts need not have these IDs. Numeric ownership is authoritative.
    info.uname = info.gname = ""
    info.pax_headers = {}


def records(database: bytes) -> dict[str, tuple[bytes, list[str], list[str], list[str]]]:
    result = {}
    for raw in database.split(b"\n\n"):
        if not raw:
            continue
        fields = raw.decode().splitlines()
        name = next((line[2:] for line in fields if line.startswith("P:")), None)
        if name is None or name in result:
            raise ValueError("invalid or duplicate installed package record")
        directory = ""
        files = []
        for line in fields:
            if line.startswith("F:"):
                directory = line[2:]
            elif line.startswith("R:"):
                files.append(f"{directory}/{line[2:]}")
        result[name] = (
            raw, files,
            [item for line in fields if line.startswith("D:") for item in line[2:].split()],
            [item for line in fields if line.startswith("p:") for item in line[2:].split()],
        )
    return result


def normalized(dependency: str) -> str:
    return re.split(r"[<>=~]", dependency, maxsplit=1)[0]


def closure(installed: dict, seeds: tuple[str, ...]) -> list[str]:
    providers: dict[str, list[str]] = {}
    for name, (_, _, _, provided) in installed.items():
        for item in provided:
            providers.setdefault(normalized(item), []).append(name)
    selected = set()
    pending = list(seeds)
    while pending:
        name = pending.pop()
        if name in selected:
            continue
        if name not in installed:
            candidates = providers.get(name, [])
            if len(candidates) != 1:
                raise ValueError(f"dependency {name} has {len(candidates)} providers: {candidates}")
            name = candidates[0]
        if name in selected:
            continue
        if name.startswith(("epiphany", "libreoffice", "webkit")):
            raise ValueError(f"unexpected desktop dependency: {name}")
        selected.add(name)
        for dep in installed[name][2]:
            if dep.startswith("!"):
                continue
            pending.append(normalized(dep))
    return sorted(selected)


def checked_relative(name: str) -> str:
    if not name or any(c in name for c in "\n\r\t\\") or name.startswith("/") or \
            str(PurePosixPath(name)) != name or ".." in PurePosixPath(name).parts:
        raise ValueError(f"invalid package path: {name}")
    return name


def locked_versions() -> dict[str, tuple[str, str]]:
    result = {}
    for line in LOCK.read_text().splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[0] in ("main", "community"):
            result[fields[1]] = (fields[2], fields[3])
    return result


class DigestReader:
    def __init__(self, source):
        self.source = source
        self.digest = hashlib.sha256()

    def read(self, count):
        data = self.source.read(count)
        self.digest.update(data)
        return data


def mesa_package(path: Path):
    spec = importlib.util.spec_from_file_location(
        "live_desktop_mesa", ROOT / "pack/scripts/build_amdgpu_mesa_package.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, module.verify(path)


def icon_caches(files: dict, archives: dict, source: Path) -> dict[str, bytes]:
    """Build GTK indexes for the selected assets, not the developer root.

    APK post-install hooks generate these outside package file lists. Without
    them GTK scans every theme directory on each application launch. Copying
    the developer cache would index icons from packages we did not ship.
    """
    themes = sorted(name.removesuffix('/index.theme') for name in files
                    if name.startswith('usr/share/icons/') and name.endswith('/index.theme'))
    if not themes:
        return {}
    tool = shutil.which('gtk-update-icon-cache')
    if not tool:
        raise ValueError('gtk-update-icon-cache is required to index selected desktop icons')
    ROOT.joinpath('.artifacts').mkdir(exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix='desktop-icons-', dir=ROOT / '.artifacts') as tmp:
        stage = Path(tmp)
        opened = {}
        try:
            for name in sorted(files):
                if not any(name.startswith(theme + '/') for theme in themes):
                    continue
                if name.endswith('/icon-theme.cache'):
                    continue
                target = stage / checked_relative(name)
                target.parent.mkdir(parents=True, exist_ok=True)
                original = source / name
                if original.is_symlink():
                    link = os.readlink(original)
                    if link.startswith('/'):
                        link = os.path.relpath(link.lstrip('/'), str(PurePosixPath(name).parent))
                    target.symlink_to(link)
                elif original.is_file():
                    shutil.copyfile(original, target)
                else:
                    package = files[name]
                    if package not in opened:
                        opened[package] = tarfile.open(archives[package], 'r:gz')
                    entry = opened[package].getmember(name)
                    if entry.isfile():
                        with opened[package].extractfile(entry) as content, target.open('wb') as output:
                            shutil.copyfileobj(content, output)
                    elif entry.issym():
                        target.symlink_to(entry.linkname)
                    else:
                        raise ValueError(f'unexpected icon asset type: {name}')
            for theme in themes:
                directory = stage / theme
                subprocess.run([tool, '--force', '--index-only', str(directory)], check=True)
                subprocess.run([tool, '--validate', str(directory)], check=True)
                result[theme + '/icon-theme.cache'] = (directory / 'icon-theme.cache').read_bytes()
        finally:
            for archive in opened.values():
                archive.close()
    return result


def build(source: Path, output: Path, mesa: Path = DEFAULT_MESA) -> dict:
    mesa_api, mesa_manifest = mesa_package(mesa)
    db = source / "lib/apk/db/installed"
    installed = records(db.read_bytes())
    selected = closure(installed, SEEDS)
    locked = locked_versions()
    cache = PACKAGE_CACHE
    files = {}
    archives = {}
    for package in selected:
        version_line = next(line for line in installed[package][0].decode().splitlines()
                            if line.startswith("V:"))
        version = version_line[2:]
        if package not in locked or locked[package][0] != version:
            raise ValueError(f"package is outside the Xfce lock: {package}-{version}")
        archive = cache / f"{package}-{version}.apk"
        if not archive.is_file() or sha256(archive) != locked[package][1]:
            raise ValueError(f"cached package digest mismatch: {archive}")
        archives[package] = archive
        for name in installed[package][1]:
            checked_relative(name)
            if name not in HOST_OWNED:
                if name in files and files[name] != package:
                    raise ValueError(f"file ownership collision: {name}")
                files[name] = package
    for name in EXTRA:
        if (source / name).exists():
            files[name] = "generated"
        else:
            raise ValueError(f"missing generated desktop runtime file: {name}")
    keys = sorted((source / "etc/apk/keys").glob("*.rsa.pub"))
    if not keys:
        raise ValueError("Alpine signing keys are missing")
    for key in keys:
        files[key.relative_to(source).as_posix()] = "generated"
    # apk must see only the packages shipped by this bundle; its lock and
    # script databases are boot-time state and cannot be copied from the host.
    database = b"\n\n".join(installed[name][0] for name in selected) + b"\n\n"
    world = "".join(f"{name}={locked[name][0]}\n" for name in SEEDS).encode()
    virtual = {
        "lib/apk/db/installed": database,
        "etc/apk/world": world,
    }
    virtual.update(icon_caches(files, archives, source))
    icon_themes = [name.removesuffix('/icon-theme.cache') for name in virtual
                   if name.endswith('/icon-theme.cache')]
    icon_directories = set(icon_themes)
    for name in files:
        for theme in icon_themes:
            if name.startswith(theme + '/'):
                icon_directories.update(str(parent) for parent in PurePosixPath(name).parents
                                        if str(parent).startswith(theme))
    for name in virtual:
        files[name] = "generated"
    overrides = {}
    for entry in mesa_manifest["entries"]:
        name = checked_relative(entry["path"].lstrip("/"))
        if name in HOST_OWNED:
            continue
        overrides[name] = mesa / "rootfs" / name
        files.setdefault(name, "pacha-mesa")
    # One authoritative Mesa closure makes importing desktop and the GPU
    # smoke archive order-independent. Keep Alpine's original APK checksums:
    # apk audit should correctly report these documented local replacements,
    # not falsely describe the custom Mesa build as pristine Alpine files.
    output.parent.mkdir(parents=True, exist_ok=True)
    payload_bytes = 0
    fallback_count = 0
    inventory = {}
    links = {}
    package_tars = {}
    accounts = target_account_ids()
    # Fix the gzip header as well as tar metadata so a source-identical
    # bundle keeps the same digest across rebuilds and USB updates.
    with output.open("wb") as compressed_output, gzip.GzipFile(
            filename="", mode="wb", fileobj=compressed_output,
            compresslevel=6, mtime=0) as gzip_output, tarfile.open(
                fileobj=gzip_output, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for name in sorted(files):
            path = overrides.get(name, source / name)
            if name in virtual:
                content = virtual[name]
                info = tarfile.TarInfo(name)
                info.size = len(content)
                info.mode = 0o644
                info.uid = info.gid = info.mtime = 0
                archive.addfile(info, io.BytesIO(content))
                inventory[name] = hashlib.sha256(content).hexdigest()
                payload_bytes += len(content)
                continue
            if path.is_file() or path.is_symlink():
                info = archive.gettarinfo(str(path), name)
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ""
                info.pax_headers = {}
                package = files[name]
                if package in archives and name not in overrides:
                    if package not in package_tars:
                        package_tars[package] = tarfile.open(archives[package], 'r:gz')
                    preserve_package_metadata(info, package_tars[package].getmember(name), accounts)
                target = mesa_api.marker_target(path)
                if target is not None:
                    if any(c in target for c in "\n\r\t"):
                        raise ValueError(f"invalid link target: {name}")
                    mesa_api.link_destination(name, target)
                    info.type = tarfile.SYMTYPE
                    info.size = 0
                    info.linkname = target
                    links[name] = target
                    archive.addfile(info)
                elif info.isfile():
                    payload_bytes += info.size
                    with path.open("rb") as src:
                        hashed = DigestReader(src)
                        archive.addfile(info, hashed)
                        inventory[name] = hashed.digest.hexdigest()
                else:
                    archive.addfile(info)
                continue
            # The developer fixture intentionally removes build tools and
            # other package-owned files. Recover an owned path from its exact
            # locked APK so the filtered apk database stays truthful.
            package = files[name]
            if package == "generated":
                raise ValueError(f"missing generated file: {path}")
            if package not in package_tars:
                package_tars[package] = tarfile.open(archives[package], "r:gz")
            package_tar = package_tars[package]
            if package_tar:
                try:
                    info = package_tar.getmember(name)
                except KeyError as exc:
                    raise ValueError(f"missing selected file in {package}: {name}") from exc
                info.name = name
                preserve_package_metadata(info, info, accounts)
                if info.isfile():
                    payload_bytes += info.size
                    with package_tar.extractfile(info) as src:
                        hashed = DigestReader(src)
                        archive.addfile(info, hashed)
                        inventory[name] = hashed.digest.hexdigest()
                else:
                    if info.issym():
                        mesa_api.link_destination(name, info.linkname)
                        if any(c in info.linkname for c in "\n\r\t"):
                            raise ValueError(f"invalid link target: {name}")
                        links[name] = info.linkname
                    elif info.islnk():
                        raise ValueError(f"unsupported APK hardlink: {name}")
                    else:
                        raise ValueError(f"unexpected APK entry type: {name}")
                    archive.addfile(info)
            fallback_count += 1
        # Tar normally creates implicit parent directories at extraction time.
        # GTK then sees a newer theme directory than its epoch-normalized
        # cache and discards the index. Restore directory metadata after all
        # children, including on BusyBox tar which restores it immediately.
        for name in sorted(icon_directories, key=lambda name: (-name.count('/'), name)):
            info = tarfile.TarInfo(name)
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            info.uid = info.gid = info.mtime = 0
            archive.addfile(info)
        for name, content in {
            "usr/share/pacha/live-desktop-files.sha256": "".join(
                f"{value}  {name}\n" for name, value in sorted(inventory.items())).encode(),
            "usr/share/pacha/live-desktop-links": "".join(
                f"{name}\t{target}\n" for name, target in sorted(links.items())).encode(),
        }.items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            info.mode = 0o644
            archive.addfile(info, io.BytesIO(content))
            payload_bytes += len(content)
    for package_tar in package_tars.values():
        package_tar.close()
    manifest = {
        "source": "Alpine v3.22 locked Xfce packages",
        "roots": list(SEEDS), "package_count": len(selected),
        "packages": selected, "file_count": len(files) + len(icon_directories) + 2,
        "icon_cache_count": len(icon_themes), "icon_directory_count": len(icon_directories),
        "uncompressed_file_bytes": payload_bytes,
        "restored_from_locked_apks": fallback_count,
        "mesa_overrides": {name: sha256(path) for name, path in sorted(overrides.items())},
        "regular_file_count": len(inventory), "symlink_count": len(links),
        "archive_bytes": output.stat().st_size,
        "sha256": sha256(output),
    }
    output.with_suffix(output.suffix + ".json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--mesa-package", type=Path, default=DEFAULT_MESA)
    args = parser.parse_args()
    result = build(args.source, args.output, args.mesa_package)
    print(f"desktop bundle: {result['package_count']} packages, "
          f"{result['file_count']} files, {result['archive_bytes']} archive bytes, "
          f"sha256={result['sha256']}")


if __name__ == "__main__":
    main()
