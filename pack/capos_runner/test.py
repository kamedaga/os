from __future__ import annotations

import subprocess
from dataclasses import dataclass

from .config import Workspace
from .qemu import qemu_plan


@dataclass(frozen=True)
class TestResult:
    serial_log: str
    matched: str


def run_smoke_test(workspace: Workspace, *, timeout: int = 30, expect: str = "bootfs ready") -> TestResult:
    test_dir = workspace.artifacts_dir / "tests" / "smoke"
    test_dir.mkdir(parents=True, exist_ok=True)
    serial_log = test_dir / "serial.log"
    plan = qemu_plan(workspace, headless=True, serial_log=serial_log)
    command = ["timeout", str(timeout), *plan.command]
    completed = subprocess.run(command, cwd=workspace.root)
    if completed.returncode not in {0, 124}:
        raise RuntimeError(f"qemu smoke test failed with exit code {completed.returncode}")
    contents = serial_log.read_text(encoding="utf-8", errors="replace") if serial_log.exists() else ""
    if expect not in contents:
        raise RuntimeError(f"expected {expect!r} in {serial_log}")
    return TestResult(serial_log=str(serial_log), matched=expect)
