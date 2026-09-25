#!/usr/bin/env python3
"""Fetch, verify, extract, and deliberately refresh locked Alpine APKs.

Normal builds use ``materialize`` and never consult APKINDEX.  ``refresh`` is
an explicit maintenance operation which rewrites an existing lock to the
versions currently published by the selected fixed Alpine branch.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


DEFAULT_MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
VALID_FIELD = re.compile(r"^[A-Za-z0-9_.+:@~-]+$")
VALID_HASH = re.compile(r"^[0-9a-f]{64}$")


@dataclasses.dataclass(frozen=True)
class Package:
    section: str
    name: str
    version: str
    sha256: str

    @property
    def filename(self) -> str:
        return f"{self.name}-{self.version}.apk"


def read_lock(path: Path) -> tuple[list[str], list[Package]]:
    comments: list[str] = []
    packages: list[Package] = []
    seen: set[str] = set()
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            comments.append(raw)
            continue
        fields = line.split()
        if len(fields) != 4:
            raise SystemExit(f"{path}:{number}: expected section package version sha256")
        package = Package(*fields)
        if package.section not in {"main", "community"}:
            raise SystemExit(f"{path}:{number}: unsupported section {package.section!r}")
        if not all(VALID_FIELD.fullmatch(value) for value in fields[:3]):
            raise SystemExit(f"{path}:{number}: invalid package field")
        if not VALID_HASH.fullmatch(package.sha256):
            raise SystemExit(f"{path}:{number}: invalid sha256")
        if package.name in seen:
            raise SystemExit(f"{path}:{number}: duplicate package {package.name}")
        seen.add(package.name)
        packages.append(package)
    return comments, packages


def download(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".download")
    temporary.unlink(missing_ok=True)
    try:
        with urllib.request.urlopen(url) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def package_url(mirror: str, branch: str, arch: str, package: Package) -> str:
    return "/".join(
        [mirror.rstrip("/"), branch, package.section, arch, package.filename]
    )


def ensure_package(
    package: Package, *, mirror: str, branch: str, arch: str, cache: Path
) -> Path:
    apk = cache / package.filename
    if not apk.is_file() or digest(apk) != package.sha256:
        download(package_url(mirror, branch, arch, package), apk)
    actual = digest(apk)
    if actual != package.sha256:
        apk.unlink(missing_ok=True)
        raise SystemExit(
            f"checksum mismatch for {package.filename}: expected {package.sha256}, got {actual}"
        )
    return apk


def selected(packages: list[Package], names: list[str]) -> list[Package]:
    if not names:
        return packages
    by_name = {package.name: package for package in packages}
    missing = sorted(set(names) - by_name.keys())
    if missing:
        raise SystemExit("packages absent from lock: " + ", ".join(missing))
    return [by_name[name] for name in names]


def safe_member_name(raw: str) -> str | None:
    name = raw.removeprefix("./")
    if name in {"", "."} or name.startswith(".SIGN.") or name in {
        ".PKGINFO",
        ".INSTALL",
    }:
        return None
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise SystemExit(f"unsafe APK member: {raw}")
    return name


def extract_apk(apk: Path, destination: Path) -> None:
    # APK v2 is a sequence of gzip-compressed tar streams.  GNU tar handles
    # this format and is already part of the pinned build environment.
    listing = subprocess.run(
        ["tar", "--warning=no-unknown-keyword", "-tzf", str(apk)],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout.splitlines()
    for member in listing:
        safe_member_name(member)
    subprocess.run(
        ["tar", "--warning=no-unknown-keyword", "-xzf", str(apk), "-C", str(destination)],
        check=True,
    )
    for metadata in destination.glob(".SIGN.*"):
        metadata.unlink()
    for name in (".PKGINFO", ".INSTALL"):
        (destination / name).unlink(missing_ok=True)


def materialize(args: argparse.Namespace) -> None:
    _, packages = read_lock(args.lock)
    packages = selected(packages, args.package)
    args.cache.mkdir(parents=True, exist_ok=True)
    if args.output:
        parent = args.output.parent
        parent.mkdir(parents=True, exist_ok=True)
        temporary = Path(tempfile.mkdtemp(prefix=args.output.name + ".", dir=parent))
    else:
        temporary = None
    try:
        for package in packages:
            apk = ensure_package(
                package,
                mirror=args.mirror,
                branch=args.branch,
                arch=args.arch,
                cache=args.cache,
            )
            if temporary is not None:
                extract_apk(apk, temporary)
        if temporary is not None:
            old = args.output.with_name(args.output.name + ".old")
            if old.exists() or old.is_symlink():
                if old.is_dir() and not old.is_symlink():
                    shutil.rmtree(old)
                else:
                    old.unlink()
            if args.output.exists() or args.output.is_symlink():
                os.replace(args.output, old)
            os.replace(temporary, args.output)
            temporary = None
            if old.exists() or old.is_symlink():
                if old.is_dir() and not old.is_symlink():
                    shutil.rmtree(old)
                else:
                    old.unlink()
    finally:
        if temporary is not None:
            shutil.rmtree(temporary, ignore_errors=True)


def parse_index(path: Path, section: str) -> dict[str, tuple[str, str]]:
    result: dict[str, tuple[str, str]] = {}
    current: dict[str, str] = {}
    for line in path.read_text(errors="strict").splitlines() + [""]:
        if not line:
            if "P" in current and "V" in current:
                result[current["P"]] = (section, current["V"])
            current = {}
            continue
        if len(line) >= 3 and line[1] == ":":
            current[line[0]] = line[2:]
    return result


def fetch_index(mirror: str, branch: str, arch: str, section: str, work: Path) -> Path:
    archive = work / f"{section}-APKINDEX.tar.gz"
    download(
        "/".join([mirror.rstrip("/"), branch, section, arch, "APKINDEX.tar.gz"]),
        archive,
    )
    with tarfile.open(archive, "r:gz") as source:
        member = source.getmember("APKINDEX")
        extracted = source.extractfile(member)
        if extracted is None:
            raise SystemExit(f"{archive}: missing APKINDEX")
        output = work / f"{section}-APKINDEX"
        output.write_bytes(extracted.read())
        return output


def refresh(args: argparse.Namespace) -> None:
    comments, old_packages = read_lock(args.lock)
    with tempfile.TemporaryDirectory(prefix="pacgo-alpine-index-") as raw_work:
        work = Path(raw_work)
        available: dict[str, tuple[str, str]] = {}
        for section in ("main", "community"):
            available.update(parse_index(fetch_index(args.mirror, args.branch, args.arch, section, work), section))
        refreshed: list[Package] = []
        args.cache.mkdir(parents=True, exist_ok=True)
        for old in old_packages:
            if old.name not in available:
                raise SystemExit(f"{old.name}: no longer published in {args.branch}")
            section, version = available[old.name]
            candidate = Package(section, old.name, version, "0" * 64)
            apk = args.cache / candidate.filename
            download(package_url(args.mirror, args.branch, args.arch, candidate), apk)
            refreshed.append(dataclasses.replace(candidate, sha256=digest(apk)))
    header = [line for line in comments if not line.startswith("# package-count ")]
    while header and not header[-1].strip():
        header.pop()
    lines = header + [f"# package-count {len(refreshed)}"]
    lines.extend(
        f"{package.section} {package.name} {package.version} {package.sha256}"
        for package in refreshed
    )
    temporary = args.lock.with_name(args.lock.name + ".tmp")
    temporary.write_text("\n".join(lines) + "\n")
    os.replace(temporary, args.lock)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--lock", type=Path, required=True)
    common.add_argument("--branch", default="v3.22")
    common.add_argument("--arch", default="x86_64")
    common.add_argument("--mirror", default=os.environ.get("ALPINE_MIRROR", DEFAULT_MIRROR))
    common.add_argument("--cache", type=Path, required=True)
    commands = result.add_subparsers(dest="command", required=True)
    stage = commands.add_parser("materialize", parents=[common])
    stage.add_argument("--package", action="append", default=[])
    stage.add_argument("--output", type=Path)
    stage.set_defaults(run=materialize)
    update = commands.add_parser("refresh", parents=[common])
    update.set_defaults(run=refresh)
    return result


def main() -> int:
    args = parser().parse_args()
    args.run(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
