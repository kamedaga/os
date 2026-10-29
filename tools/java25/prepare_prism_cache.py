#!/usr/bin/env python3
"""Prepare Prism's cache on the host after prepare_minecraft.py has finished."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile
import urllib.request

from prepare_minecraft import OUT, GAME, allowed, fetch


def main():
    staging = OUT / "prism-cache-root"
    data = staging / "root/.local/share/PrismLauncher"
    files = []
    aliases = []
    downloads = []
    baseline = {entry["path"] for entry in json.loads((OUT / "files.json").read_text())}
    by_hash = {}
    for path in (GAME / "libraries").rglob("*.jar"):
        by_hash[hashlib.sha1(path.read_bytes()).hexdigest()] = path
    by_hash[hashlib.sha1((GAME / "client.jar").read_bytes()).hexdigest()] = GAME / "client.jar"
    for uid, version, digest in [
        ("net.minecraft", "26.1.2", "441d3115f7d22f540d6c5e3908d98a3d690d3ffa22d81d8828795f07e8c2e6cd"),
        ("org.lwjgl3", "3.4.1", "4108a1aaaf182a397b588d9ee086a7473d1bc15407169475676740e04da6966b"),
    ]:
        base = "https://meta.prismlauncher.org/v1/" + uid + "/"
        directory = data / "meta" / uid
        directory.mkdir(parents=True, exist_ok=True)
        for filename in ["index.json", version + ".json"]:
            with urllib.request.urlopen(base + filename, timeout=60) as response:
                content = response.read()
            if filename != "index.json" and hashlib.sha256(content).hexdigest() != digest:
                raise ValueError("Prism metadata checksum mismatch: " + filename)
            path = directory / filename
            path.write_bytes(content)
            files.append(path)
        index = json.loads((directory / "index.json").read_text())
        assert next(v for v in index["versions"] if v["version"] == version)["sha256"] == digest
        metadata = json.loads((directory / (version + ".json")).read_text())
        libraries = list(metadata["libraries"])
        if "mainJar" in metadata:
            libraries.append(metadata["mainJar"])
        for library in libraries:
            if not allowed(library):
                continue
            artifact = library["downloads"]["artifact"]
            group, name, ver, *classifier = library["name"].split(":")
            filename = name + "-" + ver + ("-" + classifier[0] if classifier else "") + ".jar"
            relative = PurePosixPath(group.replace(".", "/"), name, ver, filename)
            assert not relative.is_absolute() and ".." not in relative.parts
            source = by_hash.get(artifact["sha1"])
            if source is None:
                # Prism's LWJGL component can include additional modules
                # absent from Mojang's selected game libraries.
                source = GAME / "libraries" / relative
                fetch(source, artifact["url"], artifact["sha1"], artifact["size"])
                by_hash[artifact["sha1"]] = source
            assert source.stat().st_size == artifact["size"]
            guest_path = "/opt/minecraft/26.1.2/libraries/" + str(relative)
            guest_source = "/opt/minecraft/26.1.2/" + str(source.relative_to(GAME))
            if guest_source not in baseline and source not in downloads:
                downloads.append(source)
            if guest_path != guest_source:
                aliases.append((guest_path, guest_source))
        if "logging" in metadata:
            log = metadata["logging"]["file"]
            path = staging / "opt/minecraft/26.1.2/assets/log_configs" / log["id"]
            fetch(path, log["url"], log["sha1"], log["size"])
            files.append(path)
    archive = OUT / "prism-cache.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for path in files:
            tar.add(path, arcname=str(path.relative_to(staging)))
        for path in downloads:
            tar.add(path, arcname="opt/minecraft/26.1.2/" + str(path.relative_to(GAME)))
        for name, target in aliases:
            entry = tarfile.TarInfo(name.lstrip("/"))
            entry.type = tarfile.SYMTYPE
            entry.linkname = target
            tar.addfile(entry)
    (OUT / "prism-cache-aliases.json").write_text(json.dumps(aliases, indent=2) + "\n")
    print(f"Prepared {len(aliases)} cache aliases, {len(downloads)} extra libraries and {len(files)} metadata/logging files: {archive}")


if __name__ == "__main__":
    main()
