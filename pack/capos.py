#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from capos_runner.bootfs import sync_bootfs
from capos_runner.build import build_kernel, build_userland
from capos_runner.config import find_workspace_root, load_workspace
from capos_runner.disk import ensure_disk
from capos_runner.manifest import generate_manifests
from capos_runner.qemu import run_qemu
from capos_runner.rootfs import diff_rootfs, print_rootfs_diff, sync_rootfs
from capos_runner.test import run_smoke_test
from capos_runner import ui


def cmd_plan(args: argparse.Namespace) -> int:
    ui.task("plan")
    root = find_workspace_root(Path.cwd())
    workspace = load_workspace(root)
    apps = workspace.discover_apps()
    skipped = [app for app in apps if workspace.app_is_skipped(app)]
    active = [app for app in apps if not workspace.app_is_skipped(app)]

    ui.key_values(
        "Workspace",
        [
            ("name", workspace.name),
            ("root", root),
            ("kernel", f"{workspace.kernel_dir} (step: {workspace.kernel_step})"),
            ("definition", workspace.config_path),
            ("apps", f"{len(apps)} ({len(active)} active, {len(skipped)} skipped)"),
            ("disk", f"{workspace.disk_image} ({workspace.disk_size_mib} MiB)"),
            ("artifacts", workspace.artifacts_dir),
            ("skip apps", ", ".join(workspace.skip_apps) if workspace.skip_apps else "-"),
            ("skip kinds", ", ".join(workspace.skip_kinds) if workspace.skip_kinds else "-"),
        ],
    )

    if getattr(args, "verbose", False):
        ui.table(
            "Apps",
            ["App", "State", "Kind", "Role", "Publishes"],
            (
                [
                    app.app_id,
                    "skip" if workspace.app_is_skipped(app) else "active",
                    app.kind,
                    app.role,
                    ", ".join(f"{entry.fs}:{entry.path}" for entry in app.publish) or "none",
                ]
                for app in apps
            ),
        )
    return 0


def cmd_config_path(args: argparse.Namespace) -> int:
    ui.task("config:path")
    root = find_workspace_root(Path.cwd())
    workspace = load_workspace(root)
    ui.console.print(workspace.config_path)
    return 0


def cmd_app_list(args: argparse.Namespace) -> int:
    ui.task("app:list")
    root = find_workspace_root(Path.cwd())
    workspace = load_workspace(root)
    ui.table(
        "Apps",
        ["App", "State", "Kind", "Role", "Source"],
        (
            [
                app.app_id,
                "skip" if workspace.app_is_skipped(app) else "active",
                app.kind,
                app.role,
                app.source_kind,
            ]
            for app in workspace.discover_apps()
        ),
    )
    return 0


def cmd_app_show(args: argparse.Namespace) -> int:
    ui.task(f"app:show", args.app_id)
    root = find_workspace_root(Path.cwd())
    workspace = load_workspace(root)
    app = workspace.get_app(args.app_id)
    rows = [
        ("id", app.app_id),
        ("kind", app.kind),
        ("role", app.role),
        ("definition", app.definition_path),
        ("source.kind", app.source_kind),
    ]
    rows.extend((f"source.{key}", value) for key, value in app.source.items())
    rows.extend(
        [
            ("build.output_name", app.output_name),
            ("build.target", app.target),
            ("build.optimize", app.optimize),
        ]
    )
    rows.extend((f"publish.{entry.id}", f"{entry.fs} {entry.path}") for entry in app.publish)
    if app.startup:
        rows.extend((f"startup.{key}", value) for key, value in app.startup.items())
    ui.key_values(f"App {app.app_id}", rows)
    return 0


def cmd_gen_manifests(args: argparse.Namespace) -> int:
    ui.task("gen:manifests", "dry-run" if args.dry_run else None)
    root = find_workspace_root(Path.cwd())
    workspace = load_workspace(root)
    outputs = generate_manifests(
        workspace,
        write=not args.dry_run,
        expand_dirs=args.expand_dirs and not args.no_expand_dirs,
    )
    ui.key_values(
        "Generated Manifests",
        [
            ("bootfs", outputs.bootfs),
            ("rootfs", outputs.rootfs),
            ("startup", outputs.startup),
            ("write", "no (dry-run)" if args.dry_run else "yes"),
            ("directory expansion", "yes" if args.expand_dirs and not args.no_expand_dirs else "no"),
        ],
    )
    return 0


def cmd_build_kernel(args: argparse.Namespace) -> int:
    ui.task("build:kernel")
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    result = build_kernel(workspace)
    ui.key_values(
        "Kernel",
        [
            ("dir", result.kernel_dir),
            ("step", result.step),
            ("boot image", result.bootx64),
        ],
    )
    return 0


def cmd_build_userland(args: argparse.Namespace) -> int:
    ui.task("build:userland", args.app if args.app else None)
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    result = build_userland(
        workspace,
        app_id=args.app,
        force=args.force,
        fail_on_unsupported=args.fail_on_unsupported,
    )
    ui.key_values(
        "Userland",
        [
            ("rebuilt sources", result.built),
            ("copied artifacts", result.copied),
            ("reused artifacts", result.reused),
            ("skipped apps", result.skipped),
            ("unsupported", ", ".join(result.unsupported) if result.unsupported else "-"),
        ],
    )
    return 0


def cmd_image(args: argparse.Namespace) -> int:
    ui.task("image")
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    disk = ensure_disk(workspace, fresh=args.fresh)
    ui.key_values("Image", [("disk", disk), ("fresh", "yes" if args.fresh else "no")])
    return 0


def cmd_sync_bootfs(args: argparse.Namespace) -> int:
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    ui.task("build:userland")
    build_userland(workspace, force=args.force_userland)
    ui.task("gen:manifests")
    manifests = generate_manifests(workspace, write=True, expand_dirs=args.expand_dirs and not args.no_expand_dirs)
    ui.task("sync:bootfs")
    result = sync_bootfs(workspace, manifests)
    ui.key_values(
        "Bootfs",
        [
            ("bootfs image", result.bootfs_image),
            ("esp manifest", result.esp_manifest),
            ("disk", result.disk_image),
        ],
    )
    return 0


def cmd_rootfs_diff(args: argparse.Namespace) -> int:
    ui.task("rootfs:diff")
    root = find_workspace_root(Path.cwd())
    workspace = load_workspace(root)
    diff = diff_rootfs(workspace, expand_dirs=args.expand_dirs)
    print_rootfs_diff(diff, limit=args.limit)
    return 0


def cmd_sync_rootfs(args: argparse.Namespace) -> int:
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    ui.task("build:userland")
    build_userland(workspace, force=args.force_userland)
    ui.task("gen:manifests")
    generate_manifests(workspace, write=True, expand_dirs=args.expand_dirs)
    ui.task("sync:rootfs")
    diff = sync_rootfs(workspace, limit=args.limit)
    print_rootfs_diff(diff, limit=args.limit)
    return 0


def cmd_qemu(args: argparse.Namespace) -> int:
    ui.task("qemu", "dry-run" if args.dry_run else None)
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    plan = run_qemu(workspace, dry_run=args.dry_run, headless=args.headless)
    ui.key_values("QEMU", [("ovmf vars", plan.ovmf_vars), ("qemu log", plan.qemu_log)])
    return 0


def cmd_test(args: argparse.Namespace) -> int:
    ui.task("test:smoke")
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    result = run_smoke_test(workspace, timeout=args.timeout, expect=args.expect)
    ui.key_values("Smoke Test", [("serial log", result.serial_log), ("matched", result.matched)])
    return 0


def cmd_all(args: argparse.Namespace) -> int:
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    ui.task("build:kernel")
    build_kernel(workspace)
    ui.task("build:userland")
    build_userland(workspace)
    ui.task("gen:manifests")
    manifests = generate_manifests(workspace, write=True, expand_dirs=False)
    ui.task("image")
    ensure_disk(workspace)
    ui.task("sync:bootfs")
    sync_bootfs(workspace, manifests)
    ui.task("sync:rootfs")
    sync_rootfs(workspace, limit=args.limit)
    return 0


def cmd_ci(args: argparse.Namespace) -> int:
    cmd_all(args)
    ui.task("test:smoke")
    workspace = load_workspace(find_workspace_root(Path.cwd()))
    run_smoke_test(workspace, timeout=args.timeout, expect=args.expect)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="capos", description="CapabilityOS packaging runner")
    sub = parser.add_subparsers(dest="command")

    plan = sub.add_parser("plan", help="show workspace plan")
    plan.add_argument("-v", "--verbose", action="store_true")
    plan.set_defaults(func=cmd_plan)

    config = sub.add_parser("config", help="Pack config helpers")
    config_sub = config.add_subparsers(dest="config_command")
    config_path = config_sub.add_parser("path", help="print Pack config path")
    config_path.set_defaults(func=cmd_config_path)

    app = sub.add_parser("app", help="app helpers")
    app_sub = app.add_subparsers(dest="app_command")
    app_list = app_sub.add_parser("list", help="list apps")
    app_list.set_defaults(func=cmd_app_list)
    app_show = app_sub.add_parser("show", help="show an app")
    app_show.add_argument("app_id")
    app_show.set_defaults(func=cmd_app_show)

    gen = sub.add_parser("gen", help="generate derived files")
    gen_sub = gen.add_subparsers(dest="gen_command")
    manifests = gen_sub.add_parser("manifests", help="generate bootfs/rootfs/startup manifests")
    manifests.add_argument("--dry-run", action="store_true")
    manifests.add_argument(
        "--expand-dirs",
        action="store_true",
        help="walk directory publishes and emit every file",
    )
    manifests.add_argument(
        "--no-expand-dirs",
        action="store_true",
        help="compatibility alias; directory expansion is already off by default",
    )
    manifests.set_defaults(func=cmd_gen_manifests)

    build = sub.add_parser("build", help="build artifacts")
    build_sub = build.add_subparsers(dest="build_command")
    build_kernel_parser = build_sub.add_parser("kernel", help="build kernel EFI image")
    build_kernel_parser.set_defaults(func=cmd_build_kernel)
    build_userland_parser = build_sub.add_parser("userland", help="build userland artifacts")
    build_userland_parser.add_argument("app", nargs="?")
    build_userland_parser.add_argument("--force", action="store_true")
    build_userland_parser.add_argument("--fail-on-unsupported", action="store_true")
    build_userland_parser.set_defaults(func=cmd_build_userland)

    image = sub.add_parser("image", help="ensure disk image")
    image.add_argument("--fresh", action="store_true", help="recreate the disk image")
    image.set_defaults(func=cmd_image)

    sync = sub.add_parser("sync", help="sync generated artifacts")
    sync_sub = sync.add_subparsers(dest="sync_command")
    sync_bootfs_parser = sync_sub.add_parser("bootfs", help="build BOOTFS.IMG and sync ESP")
    sync_bootfs_parser.add_argument("--force-userland", action="store_true")
    sync_bootfs_parser.add_argument("--expand-dirs", action="store_true")
    sync_bootfs_parser.add_argument("--no-expand-dirs", action="store_true")
    sync_bootfs_parser.set_defaults(func=cmd_sync_bootfs)
    sync_rootfs_parser = sync_sub.add_parser("rootfs", help="sync rootfs diff")
    sync_rootfs_parser.add_argument("--force-userland", action="store_true")
    sync_rootfs_parser.add_argument("--expand-dirs", action="store_true")
    sync_rootfs_parser.add_argument("--limit", type=int, default=200)
    sync_rootfs_parser.set_defaults(func=cmd_sync_rootfs)

    rootfs = sub.add_parser("rootfs", help="rootfs helpers")
    rootfs_sub = rootfs.add_subparsers(dest="rootfs_command")
    rootfs_diff = rootfs_sub.add_parser("diff", help="show managed rootfs diff against state")
    rootfs_diff.add_argument("--limit", type=int, default=200, help="maximum changed paths to print")
    rootfs_diff.add_argument(
        "--expand-dirs",
        action="store_true",
        help="walk directory publishes and diff each file; slower on WSL /mnt/c",
    )
    rootfs_diff.set_defaults(func=cmd_rootfs_diff)

    qemu = sub.add_parser("qemu", help="launch QEMU")
    qemu.add_argument("--dry-run", action="store_true")
    qemu.add_argument("--headless", action="store_true")
    qemu.set_defaults(func=cmd_qemu)

    test = sub.add_parser("test", help="run automated tests")
    test.add_argument("--timeout", type=int, default=30)
    test.add_argument("--expect", default="bootfs ready")
    test.set_defaults(func=cmd_test)

    all_parser = sub.add_parser("all", help="build and sync boot/rootfs")
    all_parser.add_argument("--limit", type=int, default=200)
    all_parser.set_defaults(func=cmd_all)

    ci = sub.add_parser("ci", help="build, sync, and run smoke test")
    ci.add_argument("--limit", type=int, default=200)
    ci.add_argument("--timeout", type=int, default=30)
    ci.add_argument("--expect", default="bootfs ready")
    ci.set_defaults(func=cmd_ci)

    parser.set_defaults(func=cmd_plan, verbose=False)
    return parser


def main(argv: list[str] | None = None) -> int:
    timer = ui.Timer.start()
    parser = build_parser()
    args = parser.parse_args(argv)
    if not hasattr(args, "func"):
        parser.print_help()
        return 2
    label = command_label(args)
    try:
        status = args.func(args)
        if status == 0:
            ui.success(label, timer.elapsed())
        return status
    except Exception as exc:
        ui.failure(label, timer.elapsed(), str(exc))
        return 1


def command_label(args: argparse.Namespace) -> str:
    parts = []
    for name in (
        "command",
        "config_command",
        "app_command",
        "gen_command",
        "build_command",
        "sync_command",
        "rootfs_command",
    ):
        value = getattr(args, name, None)
        if value:
            parts.append(str(value))
    return ":".join(parts) if parts else "plan"


if __name__ == "__main__":
    raise SystemExit(main())
