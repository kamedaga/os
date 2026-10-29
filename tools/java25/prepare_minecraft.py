#!/usr/bin/env python3
"""Prepare the official Linux x86-64 Minecraft 26.1.2 files for the existing guest."""
import concurrent.futures
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile
import time
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / ".artifacts/java25/minecraft-26.1.2"
ROOT = OUT / "root"
GAME = ROOT / "opt/minecraft/26.1.2"
VERSION_SHA1 = "58e6c8191fc15a21bf55b0695a8c50e1beb8ff55"
VERSION_URL = f"https://piston-meta.mojang.com/v1/packages/{VERSION_SHA1}/26.1.2.json"


def fetch(path, url, digest, size=None):
    def valid():
        return path.is_file() and (size is None or path.stat().st_size == size) and \
            hashlib.sha1(path.read_bytes()).hexdigest() == digest
    if valid():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".download")
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as out:
                while block := response.read(1024 * 1024):
                    out.write(block)
            if (size is not None and temporary.stat().st_size != size) or \
                    hashlib.sha1(temporary.read_bytes()).hexdigest() != digest:
                raise ValueError(f"download checksum mismatch: {path.name}")
            temporary.replace(path)
            return
        except Exception:
            temporary.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def allowed(library):
    # This installation has one target: Linux x86-64, with no launcher features.
    result = not library.get("rules")
    for rule in library.get("rules", []):
        os_rule = rule.get("os", {})
        if os_rule.get("name", "linux") != "linux":
            continue
        if os_rule.get("arch", "amd64") not in ("amd64", "x86_64"):
            continue
        if rule.get("features"):
            continue
        if "version" in os_rule:
            raise ValueError("unexpected OS-version rule in pinned metadata")
        result = rule["action"] == "allow"
    return result and not library["name"].endswith(":linux-aarch_64")


def main():
    GAME.mkdir(parents=True, exist_ok=True)
    version_path = OUT / "version.json"
    fetch(version_path, VERSION_URL, VERSION_SHA1)
    version = json.loads(version_path.read_text())
    assert version["id"] == "26.1.2" and version["javaVersion"]["majorVersion"] == 25
    tasks = []
    classpath = []
    selected = []
    for library in version["libraries"]:
        if not allowed(library):
            continue
        artifact = library["downloads"]["artifact"]
        relative = PurePosixPath(artifact["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("unsafe library path")
        path = GAME / "libraries" / relative
        tasks.append((path, artifact["url"], artifact["sha1"], artifact["size"]))
        classpath.append("/opt/minecraft/26.1.2/libraries/" + str(relative))
        selected.append((library, path))
    client = version["downloads"]["client"]
    tasks.append((GAME / "client.jar", client["url"], client["sha1"], client["size"]))
    classpath.append("/opt/minecraft/26.1.2/client.jar")
    index = version["assetIndex"]
    index_path = GAME / "assets/indexes" / (index["id"] + ".json")
    fetch(index_path, index["url"], index["sha1"], index["size"])
    objects = {obj["hash"]: obj for obj in json.loads(index_path.read_text())["objects"].values()}
    for digest, obj in objects.items():
        if len(digest) != 40 or any(c not in "0123456789abcdef" for c in digest):
            raise ValueError("unsafe asset hash")
        suffix = digest[:2] + "/" + digest
        tasks.append((GAME / "assets/objects" / suffix,
                      "https://resources.download.minecraft.net/" + suffix, digest, obj["size"]))
    print(f"Preparing {len(selected)} libraries, client and {len(objects)} assets", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        for i, _ in enumerate(pool.map(lambda task: fetch(*task), tasks), 1):
            if i % 500 == 0:
                print(f"Verified {i}/{len(tasks)} downloads", flush=True)
    natives = GAME / "natives"
    natives.mkdir(exist_ok=True)
    native_files = {}
    for library, path in selected:
        if not library["name"].endswith(":natives-linux"):
            continue
        with zipfile.ZipFile(path) as jar:
            for member in jar.namelist():
                if not member.endswith(".so"):
                    continue
                name = PurePosixPath(member).name
                contents = jar.read(member)
                if name in native_files and native_files[name] != hashlib.sha256(contents).hexdigest():
                    raise ValueError(f"conflicting native library: {name}")
                native_files[name] = hashlib.sha256(contents).hexdigest()
                (natives / name).write_bytes(contents)
    (GAME / "classpath").write_text(":".join(classpath) + "\n")
    (GAME / "version.json").write_bytes(version_path.read_bytes())
    # Prism will own authentication and launch arguments in the guest.
    (ROOT / "usr/local/bin/minecraft").unlink(missing_ok=True)
    for source, target in [(REPO / "tools/java25/java", ROOT / "usr/local/bin/java")]:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
        target.chmod(0o755)
    inventory = [{"path": "/" + str(path.relative_to(ROOT)), "bytes": path.stat().st_size,
                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                 for path in sorted(ROOT.rglob("*")) if path.is_file()]
    (OUT / "files.json").write_text(json.dumps(inventory, indent=2) + "\n")
    with tarfile.open(OUT / "minecraft.tar.gz", "w:gz", compresslevel=1) as tar:
        for entry in inventory:
            tar.add(ROOT / entry["path"].lstrip("/"), arcname=entry["path"].lstrip("/"))
    print(json.dumps({"files": len(inventory), "bytes": sum(x["bytes"] for x in inventory),
                      "native_libraries": native_files, "archive": str(OUT / "minecraft.tar.gz")}), flush=True)


if __name__ == "__main__":
    main()
