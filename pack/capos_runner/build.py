from __future__ import annotations

import filecmp
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .config import AppDefinition, Workspace
from . import ui


@dataclass(frozen=True)
class KernelBuildResult:
    kernel_dir: Path
    step: str
    bootx64: Path


@dataclass(frozen=True)
class UserlandBuildResult:
    built: int
    copied: int
    reused: int
    skipped: int
    unsupported: list[str]


def build_kernel(workspace: Workspace) -> KernelBuildResult:
    cache_root = linux_cache_root(workspace) / "zig" / "kernel"
    local_cache = cache_root / "local"
    global_cache = cache_root / "global"
    local_cache.mkdir(parents=True, exist_ok=True)
    global_cache.mkdir(parents=True, exist_ok=True)

    run(
        [
            "zig",
            "build",
            "--cache-dir",
            str(local_cache),
            "--global-cache-dir",
            str(global_cache),
            workspace.kernel_step,
        ],
        cwd=workspace.kernel_dir,
        label="build kernel",
    )
    bootx64 = workspace.kernel_dir / "zig-out" / "bin" / "EFI" / "BOOT" / "BOOTX64.EFI"
    require_nonempty_file(bootx64, "EFI boot image")
    return KernelBuildResult(kernel_dir=workspace.kernel_dir, step=workspace.kernel_step, bootx64=bootx64)


def build_userland(
    workspace: Workspace,
    *,
    app_id: str | None = None,
    force: bool = False,
    fail_on_unsupported: bool = False,
) -> UserlandBuildResult:
    selected = workspace.discover_apps()
    if app_id is not None:
        selected = [app for app in selected if app.app_id == app_id]
        if not selected:
            raise ValueError(f"app not found: {app_id}")

    built = 0
    copied = 0
    reused = 0
    skipped = 0
    unsupported: list[str] = []

    for app in selected:
        output_path = workspace.planned_artifact_path(app)
        if workspace.app_is_skipped(app) and app_id is None:
            skipped += 1
            continue

        if app.source_kind == "file":
            source_path = workspace.root / str(app.source.get("path", ""))
            if force or not source_is_usable(source_path):
                maybe_rebuild_file_source(workspace, app)
                built += 1
            if not source_is_usable(source_path):
                raise ValueError(f"{app.app_id}: source does not exist after rebuild: {source_path}")
            if source_path.is_dir() and output_path.exists() and not force:
                reused += 1
                continue
            if copy_if_changed(source_path, output_path):
                copied += 1
            else:
                reused += 1
            continue

        if app.source_kind == "zig":
            if not force and output_path.exists() and not zig_inputs_newer_than_output(workspace, app, output_path):
                reused += 1
                continue
            build_zig_app(workspace, app, output_path)
            built += 1
            copied += 1
            continue

        unsupported.append(f"{app.app_id}:{app.source_kind}")

    if unsupported and fail_on_unsupported:
        raise ValueError("unsupported userland source(s): " + ", ".join(unsupported))
    return UserlandBuildResult(
        built=built,
        copied=copied,
        reused=reused,
        skipped=skipped,
        unsupported=unsupported,
    )


def maybe_rebuild_file_source(workspace: Workspace, app: AppDefinition) -> None:
    rebuild = app.source.get("rebuild")
    if rebuild:
        command = [str(item) for item in rebuild]
        rebuild_dir = workspace.root / str(app.source.get("rebuild_dir", "."))
        run(command, cwd=rebuild_dir, label=f"rebuild {app.app_id}")
        return

    tool = str(app.source.get("rebuild_tool", ""))
    if not tool:
        return
    args = [str(item) for item in app.source.get("rebuild_args", [])]
    if tool == "wsl":
        if args[:1] == ["-e"]:
            args = args[1:]
        if not args:
            raise ValueError(f"{app.app_id}: rebuild_tool = wsl requires rebuild_args")
        command = args
    else:
        command = [tool, *args]
    rebuild_dir = workspace.root / str(app.source.get("rebuild_dir", "."))
    run(command, cwd=rebuild_dir, label=f"rebuild {app.app_id}")


def build_zig_app(workspace: Workspace, app: AppDefinition, output_path: Path) -> None:
    target = app.target
    if not target:
        raise ValueError(f"{app.app_id}: [build].target is required for source.zig")
    entry = str(app.source.get("entry", ""))
    if not entry:
        raise ValueError(f"{app.app_id}: [source.zig].entry is required")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    cache_root = linux_cache_root(workspace) / "zig" / "userland" / app.app_id
    local_cache = cache_root / "local"
    global_cache = cache_root / "global"
    local_cache.mkdir(parents=True, exist_ok=True)
    global_cache.mkdir(parents=True, exist_ok=True)

    command = [
        "zig",
        "build-exe",
        "--name",
        Path(app.output_name).stem,
        "-target",
        target,
        "-O",
        app.optimize,
        "-mcmodel=small",
        "-mno-red-zone",
        "-fPIE",
        "-fentry=_start",
        "-z",
        "common-page-size=4096",
        "-z",
        "max-page-size=4096",
    ]
    if app.strip:
        command.append("-fstrip")
    imports = list(app.source.get("imports", []))
    module = str(app.source.get("module", ""))
    if module and module not in imports:
        imports.append(module)
    for import_name in imports:
        if import_name not in {"abi_root", "persistent_fs_layout"}:
            raise ValueError(f"{app.app_id}: unsupported Zig import {import_name!r}")
        command.extend(["--dep", import_name])
    command.append(f"-Mroot={entry}")
    if "abi_root" in imports:
        command.extend(["--dep", "persistent_fs_layout"])
        command.append("-Mabi_root=userland/programs/abi/abi_root.zig")
    if imports:
        command.append("-Mpersistent_fs_layout=userland/programs/abi/persistent_fs_layout.zig")
    command.extend(
        [
            f"-femit-bin={output_path}",
            "--cache-dir",
            str(local_cache),
            "--global-cache-dir",
            str(global_cache),
        ]
    )
    run(command, cwd=workspace.root, label=f"build {app.app_id}")
    require_nonempty_file(output_path, f"{app.app_id} artifact")


def zig_inputs_newer_than_output(workspace: Workspace, app: AppDefinition, output_path: Path) -> bool:
    output_mtime = output_path.stat().st_mtime_ns
    candidates = [
        app.definition_path,
        workspace.root / str(app.source.get("entry", "")),
        workspace.root / "userland/programs/abi/abi_root.zig",
        workspace.root / "userland/programs/abi/persistent_fs_layout.zig",
    ]
    return any(path.exists() and path.stat().st_mtime_ns > output_mtime for path in candidates)


def copy_if_changed(source: Path, destination: Path) -> bool:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        if destination.exists():
            if destination.is_dir() and directories_equal(source, destination):
                return False
            remove_path(destination)
        shutil.copytree(source, destination)
        return True
    if destination.is_file() and filecmp.cmp(source, destination, shallow=False):
        return False
    shutil.copy2(source, destination)
    return True


def directories_equal(left: Path, right: Path) -> bool:
    comparison = filecmp.dircmp(left, right)
    if comparison.left_only or comparison.right_only or comparison.diff_files or comparison.funny_files:
        return False
    return all(directories_equal(left / name, right / name) for name in comparison.common_dirs)


def remove_path(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def source_is_usable(path: Path) -> bool:
    if path.is_dir():
        return True
    return path.is_file()


def require_nonempty_file(path: Path, label: str) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f"missing {label}: {path}")


def linux_cache_root(workspace: Workspace) -> Path:
    root = os.environ.get("CAPOS_CACHE_DIR")
    if root:
        return Path(root)
    return Path(os.environ.get("XDG_CACHE_HOME", "/tmp")) / "capos" / workspace.root.name


def run(command: list[str], *, cwd: Path, label: str) -> None:
    ui.command(label, command, cwd)
    completed = subprocess.run(command, cwd=cwd)
    if completed.returncode != 0:
        raise RuntimeError(f"{label} failed with exit code {completed.returncode}")
