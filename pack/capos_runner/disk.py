from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

from .config import DiskPartition, Workspace
from . import ui

SECTOR_BYTES = 512
PARTITION_ALIGNMENT_LBA = 2048
MIB = 1024 * 1024


@dataclass(frozen=True)
class PlannedPartition:
    config: DiskPartition
    first_lba: int
    last_lba: int

    @property
    def offset_bytes(self) -> int:
        return self.first_lba * SECTOR_BYTES


def ensure_disk(workspace: Workspace, *, fresh: bool = False) -> Path:
    disk = workspace.disk_image
    disk.parent.mkdir(parents=True, exist_ok=True)
    should_create = fresh or not disk.exists()
    if not should_create:
        if disk.stat().st_size == 0:
            raise ValueError(f"disk image is empty: {disk}")
        return disk

    clear_pack_state(workspace)
    size_mib = workspace.disk_size_mib
    run(["truncate", "-s", f"{size_mib}M", str(disk)], cwd=workspace.root, label="create disk")
    run(["sgdisk", "--clear", str(disk)], cwd=workspace.root, label="partition disk")

    for partition in plan_partitions(workspace):
        start = str(partition.first_lba)
        end = str(partition.last_lba)
        typecode = "EF00" if partition.config.id == "esp" else "8300"
        name = "EFI System" if partition.config.id == "esp" else partition.config.id
        run(
            [
                "sgdisk",
                f"--new={partition.config.index}:{start}:{end}",
                f"--typecode={partition.config.index}:{typecode}",
                f"--change-name={partition.config.index}:{name}",
                str(disk),
            ],
            cwd=workspace.root,
            label=f"partition {partition.config.id}",
        )
        if is_fat(partition.config.format):
            format_fat_partition(disk, partition)
    return disk


def plan_partitions(workspace: Workspace) -> list[PlannedPartition]:
    total_sectors = workspace.disk_size_mib * MIB // SECTOR_BYTES
    last_usable = total_sectors - 34
    cursor = align_up(34, PARTITION_ALIGNMENT_LBA)
    planned: list[PlannedPartition] = []
    for config in workspace.disk_partitions:
        first_lba = align_up(cursor, PARTITION_ALIGNMENT_LBA)
        if config.grow:
            last_lba = last_usable
        else:
            if not config.size_mib:
                raise ValueError(f"partition {config.id!r} requires size_mib or grow=true")
            sectors = config.size_mib * MIB // SECTOR_BYTES
            last_lba = first_lba + sectors - 1
        if last_lba > last_usable:
            raise ValueError(f"partition {config.id!r} exceeds disk size")
        planned.append(PlannedPartition(config=config, first_lba=first_lba, last_lba=last_lba))
        cursor = last_lba + 1
    return planned


def find_partition(workspace: Workspace, partition_id: str) -> PlannedPartition:
    for partition in plan_partitions(workspace):
        if partition.config.id == partition_id:
            return partition
    raise ValueError(f"missing partition {partition_id!r}")


def format_fat_partition(disk: Path, partition: PlannedPartition) -> None:
    args = ["mformat", "-i", image_spec(disk, partition), "-v", partition.config.id[:11].upper()]
    if partition.config.format.lower() in {"fat32", "esp"}:
        args.append("-F")
    args.append("::")
    run(args, cwd=disk.parent, label=f"format {partition.config.id}")


def image_spec(disk: Path, partition: PlannedPartition) -> str:
    return f"{disk}@@{partition.offset_bytes}"


def is_fat(format_name: str) -> bool:
    return format_name.lower() in {"fat", "fat16", "fat32", "esp", "efi"}


def clear_pack_state(workspace: Workspace) -> None:
    for name in ("rootfs-state.json",):
        path = workspace.state_dir / name
        if path.exists():
            path.unlink()


def align_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def run(command: list[str], *, cwd: Path, label: str) -> None:
    ui.command(label, command, cwd)
    completed = subprocess.run(command, cwd=cwd)
    if completed.returncode != 0:
        raise RuntimeError(f"{label} failed with exit code {completed.returncode}")
