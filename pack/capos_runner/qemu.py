from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .build import require_nonempty_file
from .config import Workspace
from . import ui


@dataclass(frozen=True)
class QemuPlan:
    command: list[str]
    ovmf_vars: Path
    qemu_log: Path


def qemu_plan(workspace: Workspace, *, headless: bool = False, serial_log: Path | None = None) -> QemuPlan:
    qemu = os.environ.get("CAPOS_QEMU", "qemu-system-x86_64")
    ovmf_code = Path(os.environ.get("CAPOS_OVMF_CODE", ""))
    ovmf_vars_template = Path(os.environ.get("CAPOS_OVMF_VARS_TEMPLATE", ""))
    if not ovmf_code.is_file():
        raise ValueError("CAPOS_OVMF_CODE does not point to an OVMF code image")
    if not ovmf_vars_template.is_file():
        raise ValueError("CAPOS_OVMF_VARS_TEMPLATE does not point to an OVMF vars template")
    require_nonempty_file(workspace.disk_image, "disk image")

    run_dir = workspace.artifacts_dir / "qemu"
    run_dir.mkdir(parents=True, exist_ok=True)
    ovmf_vars = run_dir / "OVMF_VARS.fd"
    qemu_log = run_dir / "qemu.log"
    if not ovmf_vars.exists():
        shutil.copy2(ovmf_vars_template, ovmf_vars)

    serial_backend = "stdio" if serial_log is None else f"file:{serial_log}"
    command = [
        qemu,
        "-machine",
        "q35,i8042=off",
        "-smp",
        "4",
        "-m",
        "2G",
        "-no-reboot",
        "-monitor",
        "none",
        "-d",
        "guest_errors,cpu_reset",
        "-D",
        str(qemu_log),
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
        "-drive",
        f"if=pflash,format=raw,file={ovmf_vars}",
        "-drive",
        f"if=none,id=capos_disk,format=raw,file={workspace.disk_image}",
        "-device",
        "virtio-blk-pci,drive=capos_disk",
        "-serial",
        serial_backend,
    ]
    if headless:
        command.append("-display")
        command.append("none")
    else:
        command.extend(["-device", "qemu-xhci,id=xhci0", "-device", "usb-tablet,bus=xhci0.0"])
    return QemuPlan(command=command, ovmf_vars=ovmf_vars, qemu_log=qemu_log)


def run_qemu(workspace: Workspace, *, dry_run: bool = False, headless: bool = False) -> QemuPlan:
    plan = qemu_plan(workspace, headless=headless)
    ui.command("qemu", plan.command, workspace.root)
    if not dry_run:
        completed = subprocess.run(plan.command, cwd=workspace.root)
        if completed.returncode != 0:
            raise RuntimeError(f"qemu failed with exit code {completed.returncode}")
    return plan
