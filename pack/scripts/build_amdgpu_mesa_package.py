#!/usr/bin/env python3
"""Stage the pinned AMD Mesa runtime, not the desktop or live rootfs."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


REPO = Path(__file__).resolve().parents[2]
MARKER = b"CAPABILITYOS_ROOTFS_SYMLINK\n"
NEEDED = re.compile(r"\(NEEDED\).*\[([^]]+)\]")
VERSION = "25.1.9"
# The DRI and GBM modules are loaded by name, not listed in DT_NEEDED.  The
# other entries are the public interfaces used by a GBM/EGL/GLES2 client.
ENTRY_PATHS = (
    "usr/lib/libEGL.so.1",
    "usr/lib/libGLESv2.so.2",
    "usr/lib/libgbm.so.1",
    "usr/lib/libdrm.so.2",
    "usr/lib/libdrm_amdgpu.so.1",
    "usr/lib/dri/radeonsi_dri.so",
    "usr/lib/gbm/dri_gbm.so",
    "usr/share/libdrm/amdgpu.ids",
    "usr/share/drirc.d/00-mesa-defaults.conf",
    "usr/share/pacha/mesa-virgl-build.txt",
)
EXTERNAL = {"libc.musl-x86_64.so.1"}
GEARS_PATH = "usr/bin/amdgpu-mesa-gears.elf"
GEARS_SOURCE = REPO / ".artifacts/amdgpu-mesa-gears-linux.elf"


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def marker_target(path: Path) -> str | None:
    if path.is_symlink():
        return os.readlink(path)
    with path.open("rb") as source:
        if source.read(len(MARKER)) != MARKER:
            return None
        target = source.read(4097)
    if not target or len(target) > 4096:
        raise ValueError(f"invalid rootfs link marker: {path}")
    return target.decode("utf-8")


def link_destination(relative: str, target: str) -> str:
    if target.startswith("/"):
        destination = posixpath.normpath(target.lstrip("/"))
    else:
        destination = posixpath.normpath(posixpath.join(posixpath.dirname(relative), target))
    if destination in ("", ".", "..") or destination.startswith("../"):
        raise ValueError(f"rootfs link escapes package: {relative} -> {target}")
    return destination


def elf_needed(path: Path) -> list[str]:
    result = subprocess.run(
        ["readelf", "-d", str(path)], check=True, capture_output=True, text=True
    )
    return sorted(set(NEEDED.findall(result.stdout)))


def check_mesa_provenance(mesa: Path) -> None:
    provenance = (mesa / "usr/share/pacha/mesa-virgl-build.txt").read_text()
    if not provenance.startswith(f"Mesa {VERSION};"):
        raise ValueError("Mesa runtime provenance is not the pinned version")
    gallium = mesa / f"usr/lib/libgallium-{VERSION}.so"
    expected = digest(gallium) + "  "
    if not any(line.startswith(expected) for line in provenance.splitlines()):
        raise ValueError("Mesa libgallium differs from its recorded build digest")


class Closure:
    def __init__(self, mesa: Path, clang: Path):
        self.roots = {"mesa": mesa, "clang": clang}
        self.selected: dict[str, tuple[str, Path]] = {}
        self.needed: dict[str, list[str]] = {}
        self.scanned: set[str] = set()

    def find(self, relative: str) -> tuple[str, Path]:
        found = [(name, root / relative) for name, root in self.roots.items()
                 if (root / relative).is_file() or (root / relative).is_symlink()]
        if not found:
            raise FileNotFoundError(f"missing Mesa dependency: /{relative}")
        if len(found) > 1 and digest(found[0][1]) != digest(found[1][1]):
            raise ValueError(f"conflicting Mesa/Clang payload: /{relative}")
        return found[0]

    def add(self, relative: str, visiting: frozenset[str] = frozenset()) -> None:
        if relative in visiting:
            raise ValueError(f"rootfs link cycle: /{relative}")
        if relative in self.scanned:
            return
        origin, source = self.find(relative)
        self.selected[relative] = origin, source
        self.scanned.add(relative)
        target = marker_target(source)
        if target is not None:
            self.add(link_destination(relative, target), visiting | {relative})
            self.needed[relative] = []
        else:
            with source.open("rb") as payload:
                is_elf = payload.read(4) == b"\x7fELF"
            if not is_elf:
                self.needed[relative] = []
                return
            dependencies = elf_needed(source)
            self.needed[relative] = dependencies
            for soname in dependencies:
                if soname in EXTERNAL:
                    continue
                self.add_library(soname)

    def add_library(self, soname: str) -> None:
        if "/" in soname or soname in ("", ".", ".."):
            raise ValueError(f"unsafe ELF dependency: {soname}")
        for directory in ("usr/lib", "lib"):
            relative = f"{directory}/{soname}"
            if any((root / relative).is_file() for root in self.roots.values()):
                self.add(relative)
                return
        raise FileNotFoundError(f"unresolved ELF dependency: {soname}")


def stage(mesa: Path, clang: Path, output: Path, live_config: Path) -> dict:
    if not (mesa / "usr/lib/libgallium-25.1.9.so").is_file():
        raise FileNotFoundError("pinned Mesa runtime missing; run tools/build_wsl_alpine_mesa.sh")
    if not (clang / "usr/lib/libLLVM.so.20.1").is_file():
        raise FileNotFoundError("Alpine LLVM runtime missing; run tools/build_wsl_alpine_clang.sh")
    if "/lib/libc.musl-x86_64.so.1:" not in live_config.read_text():
        raise ValueError("live bootfs does not provide the external musl libc")
    check_mesa_provenance(mesa)
    closure = Closure(mesa, clang)
    for relative in ENTRY_PATHS:
        closure.add(relative)
    if not GEARS_SOURCE.is_file():
        raise FileNotFoundError("Mesa gears client missing; build it first")
    gears_needed = elf_needed(GEARS_SOURCE)
    for soname in gears_needed:
        if soname not in EXTERNAL:
            closure.add_library(soname)
    if "usr/lib/libLLVM.so.20.1" not in closure.selected or \
            "usr/lib/libgallium-25.1.9.so" not in closure.selected:
        raise ValueError("radeonsi/LLVM closure is incomplete")

    output = output.resolve()
    artifacts = (REPO / ".artifacts").resolve()
    if output == artifacts or artifacts not in output.parents:
        raise ValueError("package output must be below .artifacts")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and not (output / "manifest.json").is_file():
        raise ValueError(f"refusing to replace non-package directory: {output}")
    temporary = Path(tempfile.mkdtemp(prefix="amdgpu-mesa.", dir=output.parent))
    try:
        entries = []
        for relative in sorted(closure.selected):
            origin, source = closure.selected[relative]
            destination = temporary / "rootfs" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            entries.append({"path": "/" + relative, "bytes": source.stat().st_size,
                            "sha256": digest(destination), "source": origin,
                            "needed": closure.needed[relative]})
        gears_destination = temporary / "rootfs" / GEARS_PATH
        gears_destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(GEARS_SOURCE, gears_destination)
        entries.append({"path": "/" + GEARS_PATH,
                        "bytes": GEARS_SOURCE.stat().st_size,
                        "sha256": digest(gears_destination), "source": "pacha",
                        "needed": gears_needed})
        manifest = {
            "profile": "amdgpu", "mesa_version": VERSION,
            "runtime_library_path": "/usr/lib:/lib",
            "external": sorted(EXTERNAL), "entries": entries,
            "total_bytes": sum(entry["bytes"] for entry in entries),
        }
        (temporary / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )
        verify(temporary)
        if output.exists():
            old = output.with_name(output.name + ".previous")
            if old.exists():
                raise ValueError(f"stale package backup requires inspection: {old}")
            output.rename(old)
            try:
                temporary.rename(output)
            except OSError:
                old.rename(output)
                raise
            shutil.rmtree(old)
        else:
            temporary.rename(output)
        return manifest
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def verify(output: Path) -> dict:
    manifest = json.loads((output / "manifest.json").read_text())
    if "schema" in manifest or manifest.get("profile") != "amdgpu" or \
            manifest.get("mesa_version") != VERSION or \
            manifest.get("external") != sorted(EXTERNAL):
        raise ValueError("invalid AMDGPU Mesa manifest")
    rootfs = output / "rootfs"
    listed: set[str] = set()
    total = 0
    for entry in manifest["entries"]:
        relative = entry["path"].lstrip("/")
        if not entry["path"].startswith("/") or relative.startswith("../") or \
                posixpath.normpath(relative) != relative or relative in listed:
            raise ValueError(f"invalid packaged path: {entry['path']}")
        listed.add(relative)
        path = rootfs / relative
        if path.is_symlink() or not path.is_file() or \
                path.stat().st_size != entry["bytes"] or \
                digest(path) != entry["sha256"]:
            raise ValueError(f"package file digest mismatch: {entry['path']}")
        target = marker_target(path)
        if target is not None and not (rootfs / link_destination(relative, target)).is_file():
            raise ValueError(f"missing packaged link target: {entry['path']}")
        total += entry["bytes"]
    actual = {path.relative_to(rootfs).as_posix() for path in rootfs.rglob("*")
              if path.is_file()}
    if listed != actual or total != manifest["total_bytes"]:
        raise ValueError("package file inventory differs from manifest")
    return manifest


def build_archive(package: Path, destination: Path) -> str:
    """Make a RAM-installable archive from the checked closure only."""
    manifest = verify(package)
    destination = destination.resolve()
    artifacts = (REPO / ".artifacts").resolve()
    if destination == artifacts or artifacts not in destination.parents:
        raise ValueError("archive output must be below .artifacts")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        prefix="amdgpu-mesa-", suffix=".tar.gz", dir=destination.parent,
        delete=False,
    ) as temporary:
        staging = Path(temporary.name)
    try:
        # A fixed gzip timestamp makes the optional bootfs hash stable across
        # rebuilds of an unchanged, already verified Mesa closure.
        with staging.open("wb") as raw, gzip.GzipFile(
            filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=1
        ) as compressed, tarfile.open(
            fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT
        ) as archive:
            directories: set[str] = set()
            for entry in manifest["entries"]:
                relative = entry["path"].lstrip("/")
                path = Path(relative).parent
                for parent in reversed(path.parents):
                    if str(parent) != ".":
                        directories.add(parent.as_posix())
                if str(path) != ".":
                    directories.add(path.as_posix())
            for directory in sorted(directories, key=lambda item: (item.count("/"), item)):
                info = tarfile.TarInfo(directory + "/")
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                archive.addfile(info)
            for entry in manifest["entries"]:
                relative = entry["path"].lstrip("/")
                source = package / "rootfs" / relative
                target = marker_target(source)
                info = tarfile.TarInfo(relative)
                info.mode = 0o755 if relative == GEARS_PATH else 0o644
                if target is not None:
                    link_destination(relative, target)
                    info.type = tarfile.SYMTYPE
                    info.linkname = target
                    archive.addfile(info)
                else:
                    info.size = entry["bytes"]
                    with source.open("rb") as payload:
                        archive.addfile(info, payload)
        staging.replace(destination)
    finally:
        staging.unlink(missing_ok=True)
    return digest(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesa-root", type=Path,
                        default=REPO / ".artifacts/userland-fixtures/alpine-mesa-root")
    parser.add_argument("--clang-root", type=Path,
                        default=REPO / ".artifacts/userland-fixtures/alpine-clang-root")
    parser.add_argument("--live-config", type=Path, default=REPO / "pack/live-bootfs.yaml")
    parser.add_argument("--output", type=Path,
                        default=REPO / ".artifacts/kobox2-amdgpu-mesa-package")
    parser.add_argument("--verify", action="store_true", help="verify an existing package")
    parser.add_argument("--archive-output", type=Path,
                        help="write a verified, RAM-installable tar.gz under .artifacts")
    args = parser.parse_args()
    try:
        manifest = verify(args.output) if args.verify else stage(
            args.mesa_root, args.clang_root, args.output, args.live_config)
        archive_sha256 = build_archive(args.output, args.archive_output) if args.archive_output else None
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"AMDGPU Mesa package: {error}", file=sys.stderr)
        return 1
    print(f"AMDGPU Mesa package: {len(manifest['entries'])} files, "
          f"{manifest['total_bytes']} bytes, {args.output}")
    if archive_sha256:
        print(f"AMDGPU Mesa archive: {args.archive_output} sha256={archive_sha256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
