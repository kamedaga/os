#!/usr/bin/env python3
"""Utilities for composing marker-symlink PachaOS rootfs overlays."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys


MARKER = b"CAPABILITYOS_ROOTFS_SYMLINK\n"


def link_target(path: Path) -> str | None:
    if path.is_symlink():
        return os.readlink(path)
    if not path.is_file():
        return None
    with path.open("rb") as source:
        prefix = source.read(len(MARKER))
        if prefix != MARKER:
            return None
        return source.read().decode()


def resolve_payload(path: Path, owner: Path, roots: list[Path], seen: set[Path]) -> Path | None:
    key = path.absolute()
    if key in seen:
        raise RuntimeError(f"rootfs symlink loop while resolving {path}")
    seen.add(key)

    target = link_target(path)
    if target is None:
        return path if path.is_file() else None
    candidate = owner / target.lstrip("/") if target.startswith("/") else path.parent / target
    candidate = Path(os.path.normpath(candidate))
    if candidate.exists() or candidate.is_symlink():
        return resolve_payload(candidate, owner, roots, seen)

    try:
        relative = candidate.relative_to(owner)
    except ValueError:
        relative = Path(target.lstrip("/"))
    for root in roots:
        alternate = root / relative
        if alternate.exists() or alternate.is_symlink():
            return resolve_payload(alternate, root, roots, seen)
    return None


def library_view(destination: Path, roots: list[Path]) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    for root in roots:
        for relative in (Path("lib"), Path("usr/lib")):
            directory = root / relative
            if not directory.is_dir():
                continue
            for source in sorted(directory.iterdir()):
                target = destination / source.name
                if target.exists() or target.is_symlink():
                    continue
                payload = resolve_payload(source, root, roots, set())
                if payload is not None and payload.is_file():
                    target.symlink_to(payload)


def validate_target(target: str, path: Path, owner: Path) -> None:
    resolved = owner / target.lstrip("/") if target.startswith("/") else path.parent / target
    resolved = resolved.resolve(strict=False)
    if resolved != owner and owner not in resolved.parents:
        raise RuntimeError(f"symlink escapes root: {path} -> {target}")


def deduplicate(runtime: Path, shared_roots: list[Path]) -> None:
    for link in sorted(path for path in runtime.rglob("*") if path.is_symlink()):
        target = os.readlink(link)
        validate_target(target, link, runtime)
        link.unlink()
        link.write_bytes(MARKER + target.encode())

    for path in sorted(path for path in runtime.rglob("*") if path.is_file()):
        relative = path.relative_to(runtime)
        for shared_root in shared_roots:
            shared = shared_root / relative
            if not shared.is_file():
                continue
            if path.read_bytes() != shared.read_bytes():
                raise RuntimeError(f"Xfce/shared rootfs collision differs: /{relative}")
            path.unlink()
            break

    for directory in sorted(
        (path for path in runtime.rglob("*") if path.is_dir()), reverse=True
    ):
        try:
            directory.rmdir()
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    view_parser = subparsers.add_parser("library-view")
    view_parser.add_argument("destination", type=Path)
    view_parser.add_argument("roots", type=Path, nargs="+")

    dedupe_parser = subparsers.add_parser("dedupe")
    dedupe_parser.add_argument("runtime", type=Path)
    dedupe_parser.add_argument("shared_roots", type=Path, nargs="+")

    args = parser.parse_args()
    try:
        if args.command == "library-view":
            library_view(
                args.destination.resolve(),
                [root.resolve() for root in args.roots],
            )
        else:
            deduplicate(
                args.runtime.resolve(),
                [root.resolve() for root in args.shared_roots],
            )
    except (OSError, RuntimeError, UnicodeError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
