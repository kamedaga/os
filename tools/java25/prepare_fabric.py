#!/usr/bin/env python3
"""Fetch Fabric and compatible performance mods on the host for the existing Prism instance."""
import hashlib
import json
from pathlib import Path
import tarfile
import urllib.parse
import urllib.request
import zipfile

from prepare_minecraft import OUT, fetch

GAME_VERSION = "26.1.2"
INSTANCE = "CapabilityOS-26.1.2"
PROJECTS = ["fabric-api", "sodium", "lithium", "ferrite-core", "immediatelyfast",
            "entityculling", "moreculling", "badoptimizations", "modmenu"]
USER_AGENT = "CapabilityOS-Minecraft-setup/1.0"


def read(url):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def metadata(uid, version, data_dir, records):
    base = "https://meta.prismlauncher.org/v1/" + uid + "/"
    index_bytes = read(base + "index.json")
    index = json.loads(index_bytes)
    entry = next(item for item in index["versions"] if item["version"] == version)
    content = read(base + version + ".json")
    if hashlib.sha256(content).hexdigest() != entry["sha256"]:
        raise ValueError("Prism metadata checksum mismatch: " + uid)
    directory = data_dir / "meta" / uid
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "index.json").write_bytes(index_bytes)
    (directory / (version + ".json")).write_bytes(content)
    records.append({"uid": uid, "version": version, "sha256": entry["sha256"]})
    return json.loads(content)


def main():
    output = OUT / "fabric"
    staging = output / "root"
    data = staging / "root/.local/share/PrismLauncher"
    mods = data / "instances" / INSTANCE / "minecraft/mods"
    mods.mkdir(parents=True, exist_ok=True)
    files = set()
    loaders = json.loads(read("https://meta.fabricmc.net/v2/versions/loader/" + GAME_VERSION))
    loader_version = next(item["loader"]["version"] for item in loaders if item["loader"]["stable"])
    records = []
    components = [metadata("net.fabricmc.intermediary", GAME_VERSION, data, records),
                  metadata("net.fabricmc.fabric-loader", loader_version, data, records)]
    for record in records:
        directory = data / "meta" / record["uid"]
        files.update([directory / "index.json", directory / (record["version"] + ".json")])
    libraries = []
    for component in components:
        for library in component.get("libraries", []):
            group, name, version = library["name"].split(":")
            relative = Path(group.replace(".", "/")) / name / version / (name + "-" + version + ".jar")
            url = library["url"].rstrip("/") + "/" + relative.as_posix()
            digest = read(url + ".sha1").decode().split()[0]
            if len(digest) != 40 or any(c not in "0123456789abcdefABCDEF" for c in digest):
                raise ValueError("Invalid Maven checksum: " + url)
            content = read(url)
            path = staging / "opt/minecraft" / GAME_VERSION / "libraries" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if hashlib.sha1(content).hexdigest() != digest.lower():
                raise ValueError("Library checksum mismatch: " + url)
            path.write_bytes(content)
            files.add(path)
            libraries.append({"name": library["name"], "url": url, "sha1": digest.lower()})
    selected = {}
    pending = list(PROJECTS)
    while pending:
        project = pending.pop(0)
        query = urllib.parse.urlencode({"loaders": json.dumps(["fabric"]),
                                       "game_versions": json.dumps([GAME_VERSION])})
        versions = json.loads(read("https://api.modrinth.com/v2/project/" + project + "/version?" + query))
        version = next((item for item in versions if item["version_type"] == "release"), None)
        if version is None:
            raise ValueError("No compatible stable release: " + project)
        project_id = version["project_id"]
        if project_id in selected:
            continue
        artifact = next((file for file in version["files"] if file.get("primary")), version["files"][0])
        filename = artifact["filename"]
        if Path(filename).name != filename or not filename.endswith(".jar"):
            raise ValueError("Invalid mod filename: " + filename)
        path = mods / filename
        fetch(path, artifact["url"], artifact["hashes"]["sha1"], artifact["size"])
        if hashlib.sha512(path.read_bytes()).hexdigest() != artifact["hashes"]["sha512"]:
            raise ValueError("Mod SHA512 mismatch: " + filename)
        files.add(path)
        with zipfile.ZipFile(path) as jar:
            descriptor = json.loads(jar.read("fabric.mod.json"))
        selected[project_id] = {"project": project, "project_id": project_id,
                                "version_id": version["id"], "version": version["version_number"],
                                "filename": filename, "url": artifact["url"],
                                "hashes": artifact["hashes"], "size": artifact["size"],
                                "mod_id": descriptor["id"], "depends": descriptor.get("depends", {})}
        for dependency in version["dependencies"]:
            if dependency["dependency_type"] != "required":
                continue
            if dependency.get("version_id"):
                raise ValueError("Pinned dependency needs explicit resolution: " + str(dependency))
            if dependency.get("project_id") not in selected:
                pending.append(dependency["project_id"])
        print(descriptor["id"], version["version_number"], flush=True)
    manifest = {"minecraft": GAME_VERSION, "instance": INSTANCE, "loader": loader_version,
                "components": records, "libraries": libraries, "mods": list(selected.values())}
    output.mkdir(parents=True, exist_ok=True)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "components.json").write_text(json.dumps([
        {"uid": "net.fabricmc.intermediary", "version": GAME_VERSION, "dependencyOnly": True},
        {"uid": "net.fabricmc.fabric-loader", "version": loader_version},
    ], indent=2) + "\n")
    with tarfile.open(output / "fabric.tar.gz", "w:gz") as archive:
        for path in sorted(files):
            archive.add(path, arcname=str(path.relative_to(staging)))
    print("Prepared", len(selected), "mods and", len(libraries), "libraries:", output / "fabric.tar.gz")


if __name__ == "__main__":
    main()
