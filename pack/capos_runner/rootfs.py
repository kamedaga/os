from __future__ import annotations

import hashlib
import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from .config import Workspace
from .manifest import directory_entries, discover_manifest_apps
from .disk import ensure_disk, find_partition, image_spec, is_fat, run
from . import ui


@dataclass
class RootfsDiff:
    added: dict[str, dict[str, object]] = field(default_factory=dict)
    changed: dict[str, dict[str, object]] = field(default_factory=dict)
    deleted: dict[str, dict[str, object]] = field(default_factory=dict)
    skipped_apps: list[str] = field(default_factory=list)


def diff_rootfs(workspace: Workspace, *, expand_dirs: bool = False) -> RootfsDiff:
    previous = load_state(workspace)
    desired = desired_rootfs_state(workspace, previous, expand_dirs=expand_dirs)
    return calculate_diff(previous, desired)


def sync_rootfs(workspace: Workspace, *, limit: int = 200) -> RootfsDiff:
    partition = find_partition(workspace, "rootfs")
    if not is_fat(partition.config.format):
        diff = diff_rootfs(workspace, expand_dirs=False)
        print_rootfs_diff(diff, limit=limit)
        raise ValueError(f"rootfs partition format {partition.config.format!r} is not implemented in Pack yet")

    disk = ensure_disk(workspace)
    previous = load_state(workspace)
    desired = desired_rootfs_state(workspace, previous, expand_dirs=True)
    diff = calculate_diff(previous, desired)
    spec = image_spec(disk, partition)

    for path in sorted(diff.deleted):
        delete_mtools_path(spec, path, workspace.root)
    for path, entry in sorted({**diff.added, **diff.changed}.items()):
        kind = entry.get("kind")
        if kind == "dir":
            mtools_mkdir(spec, path, workspace.root)
        elif kind == "file":
            source = Path(str(entry.get("source", "")))
            ensure_parent_dirs(spec, path, workspace.root)
            run(["mcopy", "-o", "-i", spec, str(source), f"::{path}"], cwd=workspace.root, label=f"rootfs copy {path}")

    save_state(workspace, desired)
    return diff


def desired_rootfs_state(
    workspace: Workspace,
    previous: dict[str, object],
    *,
    expand_dirs: bool,
) -> dict[str, object]:
    previous_apps = previous.get("apps", {})
    managed_paths: dict[str, dict[str, object]] = {}
    apps_state: dict[str, dict[str, object]] = {}
    skipped_apps: list[str] = []
    mode = "expanded" if expand_dirs else "summary"

    for app in discover_manifest_apps(workspace):
        app_previous = previous_apps.get(app.app_id, {}) if isinstance(previous_apps, dict) else {}
        artifact = workspace.planned_artifact_path(app)
        artifact_key = artifact_identity(artifact)
        previous_key = app_previous.get("artifact_key") if isinstance(app_previous, dict) else None
        previous_mode = app_previous.get("mode") if isinstance(app_previous, dict) else None
        previous_paths = app_previous.get("managed_paths", {}) if isinstance(app_previous, dict) else {}

        if previous_key == artifact_key and previous_mode == mode and isinstance(previous_paths, dict):
            apps_state[app.app_id] = {
                "artifact_key": artifact_key,
                "mode": mode,
                "managed_paths": previous_paths,
            }
            managed_paths.update(previous_paths)
            skipped_apps.append(app.app_id)
            continue

        app_paths: dict[str, dict[str, object]] = {}
        for publish in app.publish:
            if publish.fs != "rootfs":
                continue
            if artifact.is_dir():
                if expand_dirs:
                    for entry in directory_entries(publish.path, artifact):
                        app_paths[entry.image_path] = state_entry(entry.source_path, entry.is_dir)
                else:
                    app_paths[publish.path] = directory_state_entry(artifact)
            else:
                app_paths[publish.path] = state_entry(artifact, False)
        apps_state[app.app_id] = {
            "artifact_key": artifact_key,
            "mode": mode,
            "managed_paths": app_paths,
        }
        managed_paths.update(app_paths)

    return {
        "version": 1,
        "apps": apps_state,
        "managed_paths": managed_paths,
        "skipped_apps": skipped_apps,
    }


def calculate_diff(previous: dict[str, object], desired: dict[str, object]) -> RootfsDiff:
    old_paths = previous.get("managed_paths", {})
    new_paths = desired.get("managed_paths", {})
    diff = RootfsDiff(skipped_apps=desired.get("skipped_apps", []))
    if not isinstance(old_paths, dict) or not isinstance(new_paths, dict):
        raise ValueError("invalid rootfs state")
    for path, entry in new_paths.items():
        if path not in old_paths:
            diff.added[path] = entry
        elif comparable_entry(entry) != comparable_entry(old_paths[path]):
            diff.changed[path] = entry
    for path, entry in old_paths.items():
        if path not in new_paths:
            diff.deleted[path] = entry
    return diff


def state_path(workspace: Workspace) -> Path:
    return workspace.state_dir / "rootfs-state.json"


def load_state(workspace: Workspace) -> dict[str, object]:
    path = state_path(workspace)
    if not path.exists():
        return {"version": 1, "apps": {}, "managed_paths": {}}
    return json.loads(path.read_text(encoding="utf-8"))


def save_state(workspace: Workspace, state: dict[str, object]) -> None:
    path = state_path(workspace)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def state_entry(source: Path | None, is_dir: bool) -> dict[str, object]:
    if is_dir:
        return {"kind": "dir"}
    if source is None:
        raise ValueError("file state entry requires source")
    if not source.exists():
        return {
            "kind": "missing",
            "source": str(source),
        }
    return {
        "kind": "file",
        "source": str(source),
        "sha256": sha256_file(source),
        "size": source.stat().st_size,
        "mode": "0555" if source.name.endswith((".elf", ".ELF")) else "0444",
    }


def directory_state_entry(source: Path) -> dict[str, object]:
    if not source.exists():
        return {
            "kind": "missing-dir",
            "source": str(source),
        }
    return {
        "kind": "dir-tree",
        "source": str(source),
        "tree_key": artifact_identity(source),
    }


def artifact_identity(path: Path) -> str:
    if not path.exists():
        return f"missing:{path}"
    stat = path.stat()
    if path.is_file():
        return f"file:{path}:{stat.st_size}:{stat.st_mtime_ns}"
    if path.is_dir():
        return f"dir:{path}:{stat.st_mtime_ns}"
    return f"other:{path}:{stat.st_mtime_ns}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def comparable_entry(entry: dict[str, object]) -> dict[str, object]:
    return {
        "kind": entry.get("kind"),
        "sha256": entry.get("sha256"),
        "size": entry.get("size"),
        "mode": entry.get("mode"),
        "tree_key": entry.get("tree_key"),
    }


def print_rootfs_diff(diff: RootfsDiff, limit: int = 200) -> None:
    ui.key_values(
        "Rootfs Diff",
        [
            ("added", len(diff.added)),
            ("changed", len(diff.changed)),
            ("deleted", len(diff.deleted)),
            ("skipped unchanged apps", len(diff.skipped_apps)),
        ],
    )
    entries: list[tuple[str, str]] = []
    for label, values in (("A", diff.added), ("M", diff.changed), ("D", diff.deleted)):
        entries.extend((label, path) for path in sorted(values))
    if entries:
        ui.paths("Managed Paths", entries, limit=limit)


def ensure_parent_dirs(spec: str, image_path: str, cwd: Path) -> None:
    parent = str(Path(image_path).parent).replace("\\", "/")
    if not parent or parent == "/":
        return
    current = ""
    for part in parent.strip("/").split("/"):
        current += f"/{part}"
        mtools_mkdir(spec, current, cwd)


def mtools_mkdir(spec: str, image_dir: str, cwd: Path) -> None:
    exists = subprocess.run(
        ["mdir", "-i", spec, f"::{image_dir}"],
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=10,
    )
    if exists.returncode == 0:
        return
    completed = subprocess.run(
        ["mmd", "-i", spec, f"::{image_dir}"],
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=10,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"mkdir {image_dir} failed with exit code {completed.returncode}")


def delete_mtools_path(spec: str, image_path: str, cwd: Path) -> None:
    command = ["mdel", "-i", spec, f"::{image_path}"]
    completed = subprocess.run(command, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if completed.returncode != 0:
        subprocess.run(["mdeltree", "-i", spec, f"::{image_path}"], cwd=cwd)
