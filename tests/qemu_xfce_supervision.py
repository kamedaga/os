#!/usr/bin/env python3
"""Run one full Xfce app acceptance boot and preserve failure evidence."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import select
import socket
import time

from qemu_xfce_startup_stability import (
    DIAG_REQUEST,
    QMP,
    dump_cpu_state,
    dump_screen,
    wait_for_process_tree,
)


DESKTOP_READY = b"XFCE_APP_ACCEPTANCE_DESKTOP_READY"
ACCEPTANCE_DONE = re.compile(rb"XFCE_APP_ACCEPTANCE_DONE status=(PASS|FAIL)")
APP_RESULT = re.compile(
    rb"XFCE_APP_RESULT app=([^ ]+) iteration=([0-9]+) status=(PASS|FAIL)"
)
SESSION_EXIT = b"XFCE_STARTUP_SESSION_EXIT status="
FATAL_SERIAL = re.compile(
    rb"GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault|"
    rb"DOUBLE FAULT|KERNEL PANIC"
)


def write_result(path: Path, result: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, sort_keys=True) + "\n")


def serial_delta(path: Path, offset: int) -> tuple[bytes, int]:
    try:
        size = path.stat().st_size
    except FileNotFoundError:
        return b"", 0
    if size < offset:
        offset = 0
    with path.open("rb") as stream:
        stream.seek(offset)
        data = stream.read()
    return data, offset + len(data)


def classify(
    console: socket.socket,
    console_log,
    serial_path: Path,
    deadline: float,
) -> tuple[str, bytearray, bytes, list[dict[str, object]], bool]:
    transcript = bytearray()
    serial_seen = bytearray()
    serial_offset = 0
    app_results: list[dict[str, object]] = []
    parsed_result_count = 0
    desktop_ready = False

    while time.monotonic() < deadline:
        delta, serial_offset = serial_delta(serial_path, serial_offset)
        serial_seen.extend(delta)
        if FATAL_SERIAL.search(serial_seen):
            return (
                "kernel-fault",
                transcript,
                bytes(serial_seen),
                app_results,
                desktop_ready,
            )

        for match in list(APP_RESULT.finditer(transcript))[parsed_result_count:]:
            app_results.append(
                {
                    "app": match.group(1).decode(errors="replace"),
                    "iteration": int(match.group(2)),
                    "status": match.group(3).decode(),
                }
            )
        parsed_result_count = len(list(APP_RESULT.finditer(transcript)))
        desktop_ready = desktop_ready or DESKTOP_READY in transcript

        done = list(ACCEPTANCE_DONE.finditer(transcript))
        if done:
            # Leave a short observation window so a fault emitted alongside
            # the final guest marker is classified as the fault, not a pass.
            settle_deadline = min(deadline, time.monotonic() + 1.0)
            while time.monotonic() < settle_deadline:
                readable, _, _ = select.select(
                    [console], [], [], min(0.1, settle_deadline - time.monotonic())
                )
                if readable:
                    chunk = console.recv(65536)
                    if not chunk:
                        break
                    console_log.write(chunk)
                    console_log.flush()
                    transcript.extend(chunk)
                delta, serial_offset = serial_delta(serial_path, serial_offset)
                serial_seen.extend(delta)
                if FATAL_SERIAL.search(serial_seen):
                    return (
                        "kernel-fault",
                        transcript,
                        bytes(serial_seen),
                        app_results,
                        desktop_ready,
                    )
            status = done[-1].group(1)
            return (
                "pass" if status == b"PASS" else "app-failure",
                transcript,
                bytes(serial_seen),
                app_results,
                desktop_ready,
            )
        if SESSION_EXIT in transcript:
            return (
                "session-exit",
                transcript,
                bytes(serial_seen),
                app_results,
                desktop_ready,
            )

        readable, _, _ = select.select(
            [console], [], [], min(0.25, max(0.0, deadline - time.monotonic()))
        )
        if not readable:
            continue
        chunk = console.recv(65536)
        if not chunk:
            return (
                "console-closed",
                transcript,
                bytes(serial_seen),
                app_results,
                desktop_ready,
            )
        console_log.write(chunk)
        console_log.flush()
        transcript.extend(chunk)

    delta, _ = serial_delta(serial_path, serial_offset)
    serial_seen.extend(delta)
    if FATAL_SERIAL.search(serial_seen):
        classification = "kernel-fault"
    elif desktop_ready:
        classification = "acceptance-timeout"
    else:
        classification = "startup-timeout"
    return classification, transcript, bytes(serial_seen), app_results, desktop_ready


def main() -> int:
    timeout = float(os.environ.get("XFCE_SUPERVISION_TIMEOUT", "240"))
    diagnostic_timeout = float(
        os.environ.get("XFCE_SUPERVISION_DIAGNOSTIC_TIMEOUT", "15")
    )
    cpus = int(os.environ.get("XFCE_SUPERVISION_CPUS", "4"))
    result_path = Path(os.environ["XFCE_SUPERVISION_RESULT"])
    cpu_path = Path(os.environ["XFCE_SUPERVISION_CPU_DUMP"])
    tree_path = Path(os.environ["XFCE_SUPERVISION_PROCESS_TREE"])
    screenshot_path = Path(os.environ["XFCE_SUPERVISION_SCREENSHOT"])
    serial_path = Path(os.environ["PACGO_QEMU_SERIAL_LOG"])
    console_log_path = Path(os.environ["PACGO_QEMU_CONSOLE_LOG"])

    started = time.monotonic()
    classification = "probe-error"
    transcript = bytearray()
    serial_seen = b""
    app_results: list[dict[str, object]] = []
    desktop_ready = False
    hlt_count: int | None = None
    tree = None
    error: str | None = None
    evidence_complete = False
    qmp: QMP | None = None
    console: socket.socket | None = None

    try:
        qmp = QMP(os.environ["XFCE_SUPERVISION_QMP_SOCKET"])
        console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        console.settimeout(10.0)
        console.connect(os.environ["PACGO_QEMU_CONSOLE"])
        console.setblocking(False)
        with console_log_path.open("ab") as console_log:
            console.sendall(
                b"/bin/bash /cmd/xfce_app_acceptance.sh --startup-controller\n"
            )
            (
                classification,
                transcript,
                serial_seen,
                app_results,
                desktop_ready,
            ) = classify(
                console,
                console_log,
                serial_path,
                started + timeout,
            )

            if classification == "pass":
                evidence_complete = True
            else:
                # CPU state precedes console input because the process-tree
                # request deliberately wakes an otherwise idle guest.
                hlt_count = dump_cpu_state(qmp, cpu_path, cpus)
                dump_screen(qmp, screenshot_path)
                console.sendall(DIAG_REQUEST)
                tree = wait_for_process_tree(
                    console,
                    console_log,
                    transcript,
                    time.monotonic() + diagnostic_timeout,
                )
                if tree is not None:
                    tree_path.parent.mkdir(parents=True, exist_ok=True)
                    tree_path.write_bytes(tree)
                evidence_complete = all(
                    path.is_file() and path.stat().st_size > 0
                    for path in (cpu_path, tree_path, screenshot_path)
                )
                if not evidence_complete:
                    error = "failure evidence incomplete"
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
    finally:
        if console is not None:
            console.close()
        if qmp is not None:
            qmp.close()

    fatal_matches = sorted(
        {match.group(0).decode(errors="replace") for match in FATAL_SERIAL.finditer(serial_seen)}
    )
    result: dict[str, object] = {
        "app_results": app_results,
        "classification": classification,
        "cpus": cpus,
        "desktop_ready": desktop_ready,
        "detected_ms": round((time.monotonic() - started) * 1000, 3),
        "evidence_complete": evidence_complete,
        "fatal_serial_markers": fatal_matches,
        "hlt_count": hlt_count,
        "process_tree": str(tree_path) if tree is not None else None,
        "screenshot": str(screenshot_path) if classification != "pass" else None,
    }
    if error is not None:
        result["error"] = error
    write_result(result_path, result)
    print("XFCE_SUPERVISION_DONE " + json.dumps(result, sort_keys=True), flush=True)
    return 0 if classification == "pass" and evidence_complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
