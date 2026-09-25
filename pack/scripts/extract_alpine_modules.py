#!/usr/bin/env python3
"""Extract named kernel modules from an already locked Alpine kernel APK."""

from __future__ import annotations

import argparse
import gzip
import lzma
from pathlib import Path
import shutil
import subprocess
import tempfile

import alpine_packages


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--package", default="linux-lts")
    parser.add_argument("--branch", default="v3.22")
    parser.add_argument("--arch", default="x86_64")
    parser.add_argument("--mirror", default=alpine_packages.DEFAULT_MIRROR)
    parser.add_argument(
        "--module",
        action="append",
        required=True,
        metavar="BASENAME=OUTPUT",
        help="module basename without .ko and output filename",
    )
    args = parser.parse_args()

    _, packages = alpine_packages.read_lock(args.lock)
    matches = [package for package in packages if package.name == args.package]
    if len(matches) != 1:
        raise SystemExit(f"lock must contain exactly one {args.package} package")
    apk = alpine_packages.ensure_package(
        matches[0],
        mirror=args.mirror,
        branch=args.branch,
        arch=args.arch,
        cache=args.cache,
    )
    requested: dict[str, str] = {}
    for mapping in args.module:
        if "=" not in mapping:
            raise SystemExit(f"invalid --module {mapping!r}")
        basename, output = mapping.split("=", 1)
        requested[basename.removesuffix(".ko")] = output

    with tempfile.TemporaryDirectory(prefix="pacgo-alpine-modules-") as raw:
        root = Path(raw)
        alpine_packages.extract_apk(apk, root)
        found: dict[str, Path] = {}
        candidates = list(root.rglob("*.ko"))
        candidates += list(root.rglob("*.ko.gz"))
        candidates += list(root.rglob("*.ko.xz"))
        candidates += list(root.rglob("*.ko.zst"))
        for candidate in candidates:
            name = candidate.name
            for suffix in (".zst", ".xz", ".gz"):
                if name.endswith(suffix):
                    name = name[: -len(suffix)]
            basename = name.removesuffix(".ko").replace("-", "_")
            for wanted in requested:
                if basename == wanted.replace("-", "_"):
                    if wanted in found:
                        raise SystemExit(f"ambiguous module {wanted}: {found[wanted]} and {candidate}")
                    found[wanted] = candidate
        missing = sorted(requested.keys() - found.keys())
        if missing:
            raise SystemExit("modules absent from Alpine package: " + ", ".join(missing))
        args.output.mkdir(parents=True, exist_ok=True)
        for basename, source in found.items():
            destination = args.output / requested[basename]
            temporary = destination.with_name(destination.name + ".tmp")
            if source.name.endswith(".gz"):
                with gzip.open(source, "rb") as input_stream, temporary.open("wb") as output_stream:
                    shutil.copyfileobj(input_stream, output_stream)
            elif source.name.endswith(".xz"):
                with lzma.open(source, "rb") as input_stream, temporary.open("wb") as output_stream:
                    shutil.copyfileobj(input_stream, output_stream)
            elif source.name.endswith(".zst"):
                with temporary.open("wb") as output_stream:
                    subprocess.run(["zstd", "-q", "-d", "-c", str(source)], check=True, stdout=output_stream)
            else:
                shutil.copyfile(source, temporary)
            temporary.chmod(0o644)
            temporary.replace(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
