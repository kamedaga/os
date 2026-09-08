#!/usr/bin/env python3
"""Classify one Xfce startup and capture anomaly evidence before QEMU stops."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import select
import socket
import time

from qemu_xfce_apk_add_smoke import QMP


READY = b"XFCE_APP_ACCEPTANCE_DESKTOP_READY"
SESSION_EXIT = b"XFCE_STARTUP_SESSION_EXIT status="
TREE_BEGIN = b"XFCE_STARTUP_PROCESS_TREE_BEGIN"
TREE_END = b"XFCE_STARTUP_PROCESS_TREE_END"
DIAG_REQUEST = b"XFCE_STARTUP_DUMP_PROCESS_TREE\n"


def drain_once(console: socket.socket, log_file, transcript: bytearray, timeout: float) -> bool:
    readable, _, _ = select.select([console], [], [], max(0.0, timeout))
    if not readable:
        return True
    chunk = console.recv(65536)
    if not chunk:
        return False
    log_file.write(chunk)
    log_file.flush()
    transcript.extend(chunk)
    return True


def wait_for_startup(
    console: socket.socket,
    log_file,
    transcript: bytearray,
    deadline: float,
) -> str:
    while time.monotonic() < deadline:
        if READY in transcript:
            return "ready"
        if SESSION_EXIT in transcript:
            return "session-exit"
        if not drain_once(
            console,
            log_file,
            transcript,
            min(0.25, deadline - time.monotonic()),
        ):
            return "console-closed"
    if READY in transcript:
        return "ready"
    if SESSION_EXIT in transcript:
        return "session-exit"
    return "hang"


def dump_cpu_state(qmp: QMP, path: Path, cpus: int) -> str:
    samples: list[str] = []
    for sample_index in range(3):
        samples.append(
            str(
                qmp.execute(
                    "human-monitor-command",
                    {"command-line": "info cpus"},
                )
            )
        )
        if sample_index != 2:
            time.sleep(0.2)

    observed_cpus = len(re.findall(r"CPU #\d+", samples[0]))
    output = []
    for sample_index, sample in enumerate(samples, start=1):
        output.append(f"=== info cpus sample {sample_index} ===\n{sample.rstrip()}\n")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(output))
    if observed_cpus != cpus:
        return f"incomplete-{observed_cpus}-of-{cpus}"
    return "changed" if len(set(samples)) > 1 else "unchanged"


def dump_screen(qmp: QMP, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    qmp.execute(
        "screendump",
        {"filename": str(path.resolve()), "format": "png", "device": "pachagpu"},
    )


def render_process_tree(raw: bytes) -> bytes | None:
    records: dict[int, tuple[int, str]] = {}
    for line in raw.decode(errors="replace").splitlines():
        match = re.match(
            r"^\s*(\d+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.*)$",
            line,
        )
        if match is None:
            continue
        pid, ppid = int(match.group(1)), int(match.group(2))
        detail = (
            f"pid={pid} ppid={ppid} stat={match.group(3)} "
            f"wchan={match.group(4)} etime={match.group(5)} {match.group(6)}"
        )
        records[pid] = (ppid, detail)
    if not records:
        return None

    children: dict[int, list[int]] = {}
    roots: list[int] = []
    for pid, (ppid, _) in records.items():
        if ppid in records and ppid != pid:
            children.setdefault(ppid, []).append(pid)
        else:
            roots.append(pid)
    for child_pids in children.values():
        child_pids.sort()
    roots.sort()

    output = ["XFCE_STARTUP_PROCESS_TREE_BEGIN"]
    visited: set[int] = set()

    def visit(pid: int, prefix: str, last: bool, is_root: bool = False) -> None:
        if pid in visited:
            return
        visited.add(pid)
        branch = "" if is_root else ("`- " if last else "|- ")
        output.append(prefix + branch + records[pid][1])
        descendants = children.get(pid, [])
        child_prefix = prefix if is_root else prefix + ("   " if last else "|  ")
        for index, child in enumerate(descendants):
            visit(child, child_prefix, index == len(descendants) - 1)

    for index, root in enumerate(roots):
        visit(root, "", index == len(roots) - 1, is_root=True)
    for pid in sorted(records):
        if pid not in visited:
            visit(pid, "", True, is_root=True)
    output.append("XFCE_STARTUP_PROCESS_TREE_END")
    return ("\n".join(output) + "\n").encode()


def wait_for_process_tree(
    console: socket.socket,
    log_file,
    transcript: bytearray,
    deadline: float,
) -> bytes | None:
    while time.monotonic() < deadline:
        begin = transcript.rfind(TREE_BEGIN)
        if begin >= 0:
            end = transcript.find(TREE_END, begin + len(TREE_BEGIN))
            if end >= 0:
                line_end = transcript.find(b"\n", end)
                if line_end < 0:
                    line_end = end + len(TREE_END)
                return render_process_tree(bytes(transcript[begin:line_end] + b"\n"))
        if not drain_once(
            console,
            log_file,
            transcript,
            min(0.25, deadline - time.monotonic()),
        ):
            break
    return None


def write_result(path: Path, result: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, sort_keys=True) + "\n")


def main() -> int:
    ready_timeout = float(os.environ.get("XFCE_STARTUP_READY_TIMEOUT", "180"))
    diagnostic_timeout = float(os.environ.get("XFCE_STARTUP_DIAGNOSTIC_TIMEOUT", "15"))
    cpus = int(os.environ.get("XFCE_STARTUP_CPUS", "4"))
    result_path = Path(os.environ["XFCE_STARTUP_RESULT"])
    cpu_path = Path(os.environ["XFCE_STARTUP_CPU_INFO"])
    tree_path = Path(os.environ["XFCE_STARTUP_PROCESS_TREE"])
    screenshot_path = Path(os.environ["XFCE_STARTUP_SCREENSHOT"])

    qmp = QMP(os.environ["XFCE_STARTUP_QMP_SOCKET"])
    console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    console.settimeout(10.0)
    console.connect(os.environ["PACGO_QEMU_CONSOLE"])
    console.setblocking(False)
    transcript = bytearray()
    started = time.monotonic()
    classification = "probe-error"
    evidence_complete = False
    cpu_activity: str | None = None
    tree: bytes | None = None
    error: str | None = None
    input_attempted = False

    try:
        console_log_path = Path(os.environ["PACGO_QEMU_CONSOLE_LOG"])
        with console_log_path.open("ab") as console_log:
            command = (os.environ.get("XFCE_STARTUP_COMMAND",
                "/bin/bash /cmd/xfce_app_acceptance.sh --startup-controller") + "\n").encode()
            console.sendall(command)
            classification = wait_for_startup(
                console,
                console_log,
                transcript,
                started + ready_timeout,
            )
            detected_ms = round((time.monotonic() - started) * 1000, 3)
            if classification != "ready":
                # Snapshot the unperturbed CPU state first.  The diagnostic tty
                # request intentionally wakes the guest only after this point.
                cpu_activity = dump_cpu_state(qmp, cpu_path, cpus)
                dump_screen(qmp, screenshot_path)
                if classification == "hang":
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
                evidence_complete = (
                    cpu_path.is_file()
                    and cpu_path.stat().st_size > 0
                    and screenshot_path.is_file()
                    and screenshot_path.stat().st_size > 0
                    and tree_path.is_file()
                    and tree_path.stat().st_size > 0
                )
                if not evidence_complete:
                    error = "anomaly evidence incomplete"
            else:
                # Keep the real session alive for optional application checks;
                # readiness alone is not evidence of a usable desktop.
                application_timeout = float(os.environ.get("XFCE_STARTUP_APPLICATION_TIMEOUT", "0"))
                if application_timeout:
                    deadline = time.monotonic() + application_timeout
                    while b"XFCE_APP_ACCEPTANCE_DONE status=" not in transcript and time.monotonic() < deadline:
                        if not drain_once(console, console_log, transcript, 0.25):
                            break
                    if b"XFCE_APP_ACCEPTANCE_DONE status=PASS" not in transcript:
                        classification = "application-failure"
                        error = "application checks did not pass"
                if os.environ.get("XFCE_STARTUP_INPUT_CHECK") == "1":
                    input_attempted = True
                    # Exercise the guest's tablet/button and keyboard devices.
                    def held_keys(*codes):
                        if SESSION_EXIT in transcript:
                            raise RuntimeError("XFCE session exited during interactive checks")
                        qmp.execute("input-send-event", {"events": [
                            qmp.key_event(code, True) for code in codes]})
                        time.sleep(0.12)
                        qmp.execute("input-send-event", {"events": [
                            qmp.key_event(code, False) for code in reversed(codes)]})
                        time.sleep(0.05)

                    def held_text(value):
                        special = {" ": ("spc",), "/": ("slash",),
                            ">": ("shift", "dot"), "&": ("shift", "7"),
                            "|": ("shift", "backslash"), ";": ("semicolon",),
                            "=": ("equal",), "-": ("minus",), ".": ("dot",),
                            "_": ("shift", "minus"), "$": ("shift", "4"),
                            "?": ("shift", "slash"), "'": ("apostrophe",)}
                        events = []
                        for char in value:
                            if char in special:
                                events.append(special[char])
                            elif char.isascii() and char.isalnum():
                                events.append(("shift", char.lower()) if char.isupper() else (char,))
                            else:
                                raise ValueError(f"unsupported input character: {char!r}")
                        for codes in events:
                            held_keys(*codes)

                    qmp.execute("input-send-event", {"events": [
                        {"type": "abs", "data": {"axis": "x", "value": 1600}},
                        {"type": "abs", "data": {"axis": "y", "value": 500}},
                    ]})
                    qmp.execute("input-send-event", {"events": [
                        {"type": "btn", "data": {"button": "left", "down": True}},
                    ]})
                    time.sleep(0.12)
                    qmp.execute("input-send-event", {"events": [
                        {"type": "btn", "data": {"button": "left", "down": False}},
                    ]})
                    time.sleep(1)
                    dump_screen(qmp, screenshot_path.with_name("mouse-menu.png"))
                    held_keys("esc")
                    held_keys("ctrl", "alt", "t")
                    time.sleep(6)
                    dump_screen(qmp, screenshot_path.with_name("keyboard-terminal.png"))
                    for code in "pwd":
                        held_keys(code)
                    held_keys("ret")
                    time.sleep(1)
                    dump_screen(qmp, screenshot_path.with_name("terminal-pwd.png"))
                    terminal_command = os.environ.get("XFCE_STARTUP_TERMINAL_COMMAND")
                    if terminal_command:
                        time.sleep(2)
                        held_text(terminal_command)
                        held_keys("ret")
                        deadline = time.monotonic() + float(os.environ.get(
                            "XFCE_STARTUP_TERMINAL_COMMAND_TIMEOUT", "8"))
                        while time.monotonic() < deadline:
                            if not drain_once(console, console_log, transcript, 0.25):
                                break
                        dump_screen(qmp, screenshot_path.with_name("terminal-command.png"))
                    writer_document = os.environ.get("XFCE_STARTUP_WRITER_DOCUMENT")
                    if writer_document:
                        # Screenshots and the persisted ODT content are checked
                        # separately; injected keys alone never establish PASS.
                        def writer_pause(seconds):
                            deadline = time.monotonic() + seconds
                            while time.monotonic() < deadline:
                                if not drain_once(console, console_log, transcript, 0.25):
                                    break

                        held_text("unixd Writer save and reopen")
                        writer_pause(2)
                        dump_screen(qmp, screenshot_path.with_name("writer-edited.png"))
                        held_keys("ctrl", "s")
                        writer_pause(3)
                        dump_screen(qmp, screenshot_path.with_name("writer-save-dialog.png"))
                        held_keys("ctrl", "a")
                        held_text(writer_document)
                        held_keys("ret")
                        writer_pause(8)
                        dump_screen(qmp, screenshot_path.with_name("writer-saved.png"))
                        held_keys("ctrl", "w")
                        writer_pause(3)
                        dump_screen(qmp, screenshot_path.with_name("writer-closed.png"))
                        held_keys("ctrl", "o")
                        writer_pause(2)
                        held_text(writer_document)
                        held_keys("ret")
                        writer_pause(10)
                        dump_screen(qmp, screenshot_path.with_name("writer-reopened.png"))
                    if os.environ.get("XFCE_STARTUP_FILE_MANAGER_CHECK") == "1":
                        for code in "thunar":
                            held_keys(code)
                        held_keys("ret")
                        time.sleep(6)
                        dump_screen(qmp, screenshot_path.with_name("thunar-open.png"))
                        held_keys("ctrl", "l")
                        held_keys("slash")
                        for code in "usr":
                            held_keys(code)
                        held_keys("ret")
                        time.sleep(3)
                        dump_screen(qmp, screenshot_path.with_name("thunar-usr.png"))
                    if os.environ.get("XFCE_STARTUP_DESKTOP_CHECK") == "1":
                        held_keys("ctrl", "alt", "d")
                        deadline = time.monotonic() + 15
                        while time.monotonic() < deadline:
                            if not drain_once(console, console_log, transcript, 0.25):
                                break
                        dump_screen(qmp, screenshot_path.with_name("desktop-icons.png"))
                    while select.select([console], [], [], 0)[0]:
                        if not drain_once(console, console_log, transcript, 0):
                            break
                dump_screen(qmp, screenshot_path)
                evidence_complete = True
                manual_seconds = float(os.environ.get("XFCE_STARTUP_MANUAL_SECONDS", "0"))
                if manual_seconds:
                    # Hand the single-client QMP socket to the operator while
                    # continuing to collect console output in this same VM.
                    qmp.close()
                    ready_path = screenshot_path.with_name("manual-ready")
                    done_path = screenshot_path.with_name("manual-done")
                    ready_path.write_text("QMP released; session is not verified\n")
                    deadline = time.monotonic() + manual_seconds
                    while time.monotonic() < deadline and not done_path.exists():
                        if not drain_once(console, console_log, transcript, 0.25):
                            break
                        if SESSION_EXIT in transcript:
                            break
                    if not done_path.exists():
                        classification = "manual-incomplete"
                        error = "manual verification did not finish"
                if SESSION_EXIT in transcript:
                    classification = "session-exit"
                    error = "XFCE session exited after startup"
    except Exception as exc:
        detected_ms = round((time.monotonic() - started) * 1000, 3)
        error = f"{type(exc).__name__}: {exc}"
    finally:
        console.close()
        qmp.close()

    serial_path = os.environ.get("PACGO_QEMU_SERIAL_LOG")
    if serial_path:
        serial = Path(serial_path).read_bytes()
        if b"PAGE FAULT" in serial or b"GENERAL PROTECTION" in serial:
            classification = "guest-fault"
            error = "guest fault recorded during XFCE verification"

    result: dict[str, object] = {
        "classification": classification,
        "cpus": cpus,
        "detected_ms": detected_ms,
        "evidence_complete": evidence_complete,
        "cpu_activity": cpu_activity,
        "process_tree": str(tree_path) if tree is not None else None,
        "screenshot": str(screenshot_path) if screenshot_path.is_file() else None,
        "input_check": "attempted-unverified" if input_attempted else "not-run",
    }
    if error is not None:
        result["error"] = error
    write_result(result_path, result)
    print("XFCE_STARTUP_PROBE_DONE " + json.dumps(result, sort_keys=True), flush=True)
    return 0 if evidence_complete and classification == "ready" and error is None else 1


if __name__ == "__main__":
    raise SystemExit(main())
