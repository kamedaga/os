from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

PACK_CONFIG = Path("pack") / "pack.yaml"


@dataclass(frozen=True)
class PublishEntry:
    id: str
    fs: str
    path: str


@dataclass(frozen=True)
class DiskPartition:
    id: str
    index: int
    format: str
    size_mib: int | None = None
    grow: bool = False


@dataclass
class AppDefinition:
    definition_path: Path
    app_id: str
    kind: str
    role: str
    source_kind: str
    source: dict[str, Any]
    output_name: str
    target: str
    optimize: str
    strip: bool
    publish: list[PublishEntry] = field(default_factory=list)
    startup: dict[str, Any] | None = None


@dataclass
class Workspace:
    root: Path
    config_path: Path
    raw: dict[str, Any]

    @property
    def name(self) -> str:
        return self.raw.get("name", "CapabilityOS")

    @property
    def artifacts_dir(self) -> Path:
        artifacts = self.raw.get("artifacts", ".artifacts")
        if isinstance(artifacts, dict):
            artifacts = artifacts.get("dir", ".artifacts")
        return self.root / str(artifacts)

    @property
    def state_dir(self) -> Path:
        return self.root / self.raw.get("state", ".artifacts/pack")

    @property
    def kernel_dir(self) -> Path:
        return self.root / self.raw.get("kernel", {}).get("dir", "kernel")

    @property
    def kernel_step(self) -> str:
        return self.raw.get("kernel", {}).get("step", "efi")

    @property
    def skip_apps(self) -> list[str]:
        return list(self.raw.get("skip", {}).get("apps", []))

    @property
    def skip_kinds(self) -> list[str]:
        return list(self.raw.get("skip", {}).get("kinds", []))

    @property
    def include_skipped_artifacts_in_manifests(self) -> bool:
        return bool(self.raw.get("skip", {}).get("includeSkippedArtifacts", False))

    @property
    def disk_image(self) -> Path:
        return self.root / self.raw.get("disk", {}).get("image", ".artifacts/disk.img")

    @property
    def disk_size_mib(self) -> int:
        return int(self.raw.get("disk", {}).get("sizeMiB", 512))

    @property
    def disk_recreate(self) -> str:
        return str(self.raw.get("disk", {}).get("recreate", "if-missing"))

    @property
    def disk_partitions(self) -> list[DiskPartition]:
        partitions = []
        raw_partitions = self.raw.get("disk", {}).get("partitions", {})
        if isinstance(raw_partitions, dict):
            iterable = [{"id": key, **value} for key, value in raw_partitions.items()]
        else:
            iterable = raw_partitions
        for raw in iterable:
            size = raw.get("sizeMiB")
            partitions.append(
                DiskPartition(
                    id=str(raw.get("id", "")),
                    index=int(raw.get("index", 0)),
                    format=str(raw.get("format", "")),
                    size_mib=int(size) if size is not None else None,
                    grow=bool(raw.get("grow", False)),
                )
            )
        return sorted(partitions, key=lambda partition: partition.index)

    def disk_partition(self, partition_id: str) -> DiskPartition:
        for partition in self.disk_partitions:
            if partition.id == partition_id:
                return partition
        raise ValueError(f"missing disk partition id = {partition_id!r}")

    @property
    def manifests_dir(self) -> Path:
        return self.root / self.raw.get("manifests", {}).get("dir", ".artifacts/manifests")

    @property
    def bootfs_manifest(self) -> Path:
        return self.root / self.raw.get("manifests", {}).get("bootfs", ".artifacts/manifests/bootfs.generated.txt")

    @property
    def rootfs_manifest(self) -> Path:
        return self.root / self.raw.get("manifests", {}).get("rootfs", ".artifacts/manifests/rootfs.generated.txt")

    @property
    def startup_manifest(self) -> Path:
        return self.root / self.raw.get("manifests", {}).get("startup", ".artifacts/manifests/startup.generated.txt")

    @property
    def startup_manifest_path(self) -> str:
        return self.raw.get("startupManifest", {}).get("path", "/sys/startup_manifest.txt")

    @property
    def startup_manifest_include(self) -> list[str]:
        return list(self.raw.get("startupManifest", {}).get("include", []))

    @property
    def publish_dirs(self) -> list[dict[str, str]]:
        return [{"fs": "rootfs", "path": path} for path in self.raw.get("rootfsDirs", [])]

    def discover_apps(self) -> list[AppDefinition]:
        raw_apps = self.raw.get("apps", {})
        if isinstance(raw_apps, dict):
            apps = [load_app_definition(self.config_path, app_id, raw) for app_id, raw in raw_apps.items()]
        else:
            apps = [load_app_definition(self.config_path, str(raw.get("id", "")), raw) for raw in raw_apps]
        seen: set[str] = set()
        for app in apps:
            if app.app_id in seen:
                raise ValueError(f"duplicate app id: {app.app_id}")
            seen.add(app.app_id)
        return sorted(apps, key=lambda app: app.app_id)

    def get_app(self, app_id: str) -> AppDefinition:
        for app in self.discover_apps():
            if app.app_id == app_id:
                return app
        raise ValueError(f"app not found: {app_id}")

    def app_is_skipped(self, app: AppDefinition) -> bool:
        app_id = app.app_id.lower()
        kind = app.kind.lower()
        skip_apps = {item.lower() for item in self.skip_apps}
        skip_kinds = {item.lower() for item in self.skip_kinds}
        return app_id in skip_apps or kind in skip_kinds

    def planned_artifact_path(self, app: AppDefinition) -> Path:
        return self.artifacts_dir / "userland" / app.app_id / app.output_name


def find_workspace_root(start: Path) -> Path:
    current = start.resolve()
    while True:
        if (current / PACK_CONFIG).is_file():
            return current
        if current.parent == current:
            raise FileNotFoundError(f"could not find {PACK_CONFIG.as_posix()} from {start}")
        current = current.parent


def load_workspace(root: Path) -> Workspace:
    config_path = root / PACK_CONFIG
    raw = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"{config_path}: expected a mapping")
    if int(raw.get("schema", 0)) != 1:
        raise ValueError(f"{config_path}: unsupported schema")
    validate_workspace(config_path, raw)
    return Workspace(root=root, config_path=config_path, raw=raw)


def validate_workspace(config_path: Path, raw: dict[str, Any]) -> None:
    supported_rootfs_formats = {"fat16", "fat32", "ext4"}
    partitions = raw.get("disk", {}).get("partitions", {})
    if isinstance(partitions, dict):
        iterable = [{"id": key, **value} for key, value in partitions.items()]
    else:
        iterable = partitions
    for partition in iterable:
        partition_id = str(partition.get("id", ""))
        format_name = str(partition.get("format", "")).lower()
        if partition_id == "rootfs" and format_name not in supported_rootfs_formats:
            raise ValueError(
                f"{config_path}: rootfs format {format_name!r} is not supported by Pack; "
                "use fat32 now, or ext4 after the writer is implemented"
            )


def load_app_definition(definition_path: Path, app_id: str, raw: dict[str, Any]) -> AppDefinition:
    source_kind, source = normalize_source(raw)
    publish = normalize_publish(raw)
    app_id = str(app_id)
    if not app_id:
        raise ValueError(f"{definition_path}: app entry is missing id")
    return AppDefinition(
        definition_path=definition_path,
        app_id=app_id,
        kind=str(raw.get("kind", source_kind)),
        role=str(raw.get("role", "asset")),
        source_kind=source_kind,
        source=source,
        output_name=str(raw.get("out", "")),
        target=str(raw.get("target", "")),
        optimize=str(raw.get("optimize", "release")),
        strip=bool(raw.get("strip", False)),
        publish=publish,
        startup=raw.get("startup"),
    )


def normalize_source(raw: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    for kind in ("file", "zig", "cargo", "capc"):
        if kind not in raw:
            continue
        value = raw[kind]
        if isinstance(value, str):
            return kind, {"path": value}
        if isinstance(value, dict):
            source = dict(value)
            if "cwd" in source:
                source["rebuild_dir"] = source.pop("cwd")
            return kind, source
        raise ValueError(f"invalid {kind} source in app definition")
    return "none", {}


def normalize_publish(raw: dict[str, Any]) -> list[PublishEntry]:
    entries: list[PublishEntry] = []
    for fs in ("bootfs", "rootfs"):
        value = raw.get(fs)
        if value is None:
            continue
        entries.extend(publish_entries(fs, value))
    return entries


def publish_entries(fs: str, value: Any) -> list[PublishEntry]:
    if isinstance(value, str):
        return [PublishEntry(id=fs, fs=fs, path=value)]
    if isinstance(value, dict):
        return [PublishEntry(id=str(key), fs=fs, path=publish_path(item)) for key, item in value.items()]
    if isinstance(value, list):
        entries = []
        for index, item in enumerate(value):
            if isinstance(item, str):
                entries.append(PublishEntry(id=f"{fs}_{index}", fs=fs, path=item))
            elif isinstance(item, dict):
                item_id = str(item.get("id", f"{fs}_{index}"))
                entries.append(PublishEntry(id=item_id, fs=fs, path=publish_path(item)))
            else:
                raise ValueError(f"invalid {fs} publish entry")
        return entries
    raise ValueError(f"invalid {fs} publish section")


def publish_path(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        return str(value.get("path", ""))
    return str(value)
