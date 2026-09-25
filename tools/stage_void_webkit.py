#!/usr/bin/env python3
"""Stage signed, pinned upstream binaries; never compile or patch upstream code."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent
LOCK = REPO / "tools/manifests/void-webkit-x86_64-musl.lock"
KEY = REPO / "tools/manifests/void-linux-public.pem"
MIRROR = "https://repo-default.voidlinux.org/current/musl"
PREFIX = Path("opt/pacha/webkitgtk-2.50.4")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def copy_entry(source, destination, source_root=None, destination_root=None):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink():
        destination.unlink()
    if source.is_symlink():
        target = os.readlink(source)
        if os.path.isabs(target):
            raise ValueError(f"unexpected package library symlink: {source}: {target}")
        if source_root is None or destination_root is None:
            if "/" in target or target in (".", ".."):
                raise ValueError(f"unexpected package library symlink: {source}: {target}")
        else:
            source_target = (source.parent / target).resolve()
            destination_target = (destination.parent / target).resolve()
            if not source_target.is_relative_to(source_root.resolve()) or not destination_target.is_relative_to(
                destination_root.resolve()
            ):
                raise ValueError(f"package symlink escapes its root: {source}: {target}")
        if destination.exists():
            destination.unlink()
        destination.symlink_to(target)
    else:
        shutil.copy2(source, destination)


def install_private_library_aliases(runtime):
    # Void embeds /usr/lib64 in both Epiphany's RUNPATH and WebKit's bundle
    # path, but ships their payload under /usr/lib. Private aliases preserve
    # those unmodified binaries without changing the desktop's library search.
    for name in ("epiphany", "webkitgtk-6.0"):
        bundle_path = runtime / "usr/lib64" / name
        bundle_path.parent.mkdir(parents=True, exist_ok=True)
        if bundle_path.is_symlink() or bundle_path.is_file():
            bundle_path.unlink()
        bundle_path.symlink_to("../lib/" + name)


def stage(runtime, packages):
    for name, package in packages:
        webkit = name.startswith("libwebkitgtk60-")
        epiphany = name.startswith("epiphany-")
        if epiphany:
            # Alpine Epiphany is deliberately not retained: its older WebKit
            # integration triggers the browser bug this overlay avoids.  Copy
            # exactly the files owned by Void's matching Epiphany package;
            # dependencies remain supplied by the locked base runtime below.
            for relative in ("usr/bin", "usr/libexec", "usr/lib/epiphany", "usr/share"):
                base = package / relative
                if not base.exists():
                    continue
                for source in sorted(base.rglob("*")):
                    if source.is_file() or source.is_symlink():
                        copy_entry(source, runtime / source.relative_to(package), package, runtime)
            continue
        for source in sorted((package / "usr/lib").iterdir()):
            selected = (webkit and source.name.startswith(("libwebkitgtk-6.0.so.", "libjavascriptcoregtk-6.0.so."))) or (
                not webkit and source.name.startswith(("libicudata.so.", "libicuuc.so.", "libicui18n.so.")))
            if not selected:
                continue
            target = runtime / PREFIX / "lib" / source.name
            copy_entry(source, target, package, runtime)
            public = runtime / "usr/lib" / source.name
            public.parent.mkdir(parents=True, exist_ok=True)
            if public.exists() or public.is_symlink():
                public.unlink()
            public.symlink_to(os.path.relpath(target, public.parent))
        if webkit:
            for relative in ("usr/libexec/webkitgtk-6.0", "usr/lib/webkitgtk-6.0", "usr/lib/girepository-1.0"):
                for source in sorted((package / relative).rglob("*")):
                    if source.is_file() or source.is_symlink():
                        copy_entry(source, runtime / source.relative_to(package), package, runtime)
        elif not webkit:
            # Void builds ICU with external data, not an embedded libicudata.
            # Libraries alone load successfully but ubrk_open then fails.
            data = package / "usr/share/icu/78.3/icudt78l.dat"
            if not data.is_file() or data.is_symlink():
                raise ValueError("ICU 78 runtime data missing from package")
            for source in sorted(data.parent.rglob("*")):
                if source.is_file() or source.is_symlink():
                    copy_entry(source, runtime / source.relative_to(package), package, runtime)
        for source in sorted((package / "usr/share/licenses").rglob("*")):
            if source.is_file():
                copy_entry(
                    source,
                    runtime / PREFIX / "licenses" / source.relative_to(package / "usr/share/licenses"),
                    package,
                    runtime,
                )
    launcher = REPO / "userland/fixtures/linux/epiphany-launcher.sh"
    install_private_library_aliases(runtime)
    copy_entry(launcher, runtime / "usr/local/bin/epiphany")
    (runtime / "usr/local/bin/epiphany").chmod(0o755)
    # D-Bus activation uses an absolute executable and otherwise bypasses PATH.
    # These are rootfs launch-policy files, not upstream implementation patches.
    for directory in ("usr/share/applications", "usr/share/dbus-1/services"):
        for path in (runtime / directory).glob("*Epiphany*"):
            if path.suffix not in (".desktop", ".service"):
                continue
            lines = path.read_text().splitlines(keepends=True)
            path.write_text("".join(line.replace("Exec=/usr/bin/epiphany", "Exec=/usr/local/bin/epiphany").replace(
                "Exec=epiphany", "Exec=/usr/local/bin/epiphany") if line.startswith("Exec=") else line for line in lines))
    record = runtime / "usr/share/pacha/void-webkit-packages.lock"
    record.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(LOCK, record)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path)
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    if runtime == Path("/") or not (runtime / "usr/bin/epiphany").is_file():
        parser.error("target must be a staged root containing Alpine Epiphany")
    cache = REPO / ".artifacts/third_party/void-webkit"
    cache.mkdir(parents=True, exist_ok=True)
    zstd = shutil.which("zstd") or str(REPO / ".artifacts/tools-zstd/root/usr/bin/zstd")
    if not Path(zstd).is_file():
        parser.error("zstd is required to extract XBPS packages")
    with tempfile.TemporaryDirectory(prefix="verify-", dir=cache) as temporary:
        temporary = Path(temporary)
        packages = []
        for line in LOCK.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            name, expected = line.split()
            archive = cache / name
            if not archive.exists() or digest(archive) != expected:
                download = temporary / name
                subprocess.run(["curl", "-fSL", "--retry", "2", f"{MIRROR}/{name}", "-o", str(download)], check=True)
                if digest(download) != expected:
                    raise ValueError(f"checksum mismatch: {name}")
                download.replace(archive)
            signature = cache / (name + ".sig2")
            if not signature.exists():
                subprocess.run(["curl", "-fSL", "--retry", "2", f"{MIRROR}/{name}.sig2", "-o", str(signature)], check=True)
            subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(KEY), "-signature", str(signature), str(archive)], check=True)
            package = temporary / (name + ".root")
            package.mkdir()
            subprocess.run(["tar", "-I", zstd, "-xf", str(archive), "-C", str(package)], check=True)
            packages.append((name, package))
        stage(runtime, packages)


if __name__ == "__main__":
    main()
