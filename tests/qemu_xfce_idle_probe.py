#!/usr/bin/env python3
"""Observe an idle Xfce boot without launching another guest process."""

from __future__ import annotations

import json
import os
from pathlib import Path
import select
import socket
import time

from qemu_xfce_apk_add_smoke import QMP


def ppm_nonblack_pixels(path: Path, threshold: int = 8) -> int:
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError(f"not a P6 PPM: {path}")
    cursor = 2
    tokens: list[bytes] = []
    while len(tokens) < 3:
        while cursor < len(data) and data[cursor] in b" \t\r\n":
            cursor += 1
        if cursor < len(data) and data[cursor] == ord("#"):
            cursor = data.index(b"\n", cursor) + 1
            continue
        end = cursor
        while end < len(data) and data[end] not in b" \t\r\n":
            end += 1
        tokens.append(data[cursor:end])
        cursor = end
    if cursor >= len(data) or data[cursor] not in b" \t\r\n":
        raise ValueError(f"missing PPM pixel separator: {path}")
    cursor += 2 if data[cursor : cursor + 2] == b"\r\n" else 1
    width, height, maximum = map(int, tokens)
    pixels = data[cursor:]
    if width <= 0 or height <= 0 or maximum != 255:
        raise ValueError(f"unsupported PPM geometry: {tokens!r}")
    if len(pixels) != width * height * 3:
        raise ValueError(f"truncated PPM: {path}")
    return sum(
        1
        for offset in range(0, len(pixels), 3)
        if max(pixels[offset : offset + 3]) > threshold
    )


def read_watch_symbols(qmp, entries) -> str:
    row = []
    for entry in entries:
        name, _, addr = entry.partition("=")
        count, _, base = addr.partition("*")
        words, addr = (count, base) if base else ("1", addr)
        raw = qmp.execute(
            "human-monitor-command",
            {"command-line": f"x /{words}gx {addr}", "cpu-index": 0},
        ).strip()
        row.append(
            f"{name}={raw.split(':')[-1].strip()}"
            if words == "1"
            else f"{name}=[{raw}]"
        )
    return "  ".join(row)


def drain_console(console: socket.socket, console_log, deadline: float) -> None:
    while time.monotonic() < deadline:
        readable, _, _ = select.select(
            [console], [], [], max(0.0, min(0.25, deadline - time.monotonic()))
        )
        if not readable:
            continue
        chunk = console.recv(65536)
        if not chunk:
            return
        console_log.write(chunk)
        console_log.flush()


def main() -> int:
    wait_seconds = float(os.environ.get("XFCE_IDLE_WAIT_SECONDS", "75"))
    action = os.environ.get("XFCE_IDLE_ACTION", "")
    action_wait_seconds = float(os.environ.get("XFCE_IDLE_ACTION_WAIT_SECONDS", "20"))
    screenshot = os.environ.get("XFCE_IDLE_SCREENSHOT")
    screenshot_format = os.environ.get("XFCE_IDLE_SCREENSHOT_FORMAT", "ppm")
    result_path = os.environ.get("XFCE_IDLE_RESULT")
    cpu_dump_path = os.environ.get("XFCE_IDLE_CPU_DUMP")
    wait_for_nonblack = os.environ.get("XFCE_IDLE_WAIT_FOR_NONBLACK", "0") == "1"
    probe_interval_seconds = float(
        os.environ.get("XFCE_IDLE_PROBE_INTERVAL_SECONDS", "5")
    )
    settle_seconds = float(os.environ.get("XFCE_IDLE_SETTLE_SECONDS", "10"))
    minimum_nonblack_pixels = int(
        os.environ.get("XFCE_IDLE_MINIMUM_NONBLACK_PIXELS", "4096")
    )
    started_ns = time.monotonic_ns()
    visible_frame_ms: float | None = None
    visible_nonblack_pixels = 0

    qmp = QMP(os.environ["XFCE_IDLE_QMP_SOCKET"])
    console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    console.settimeout(10.0)
    console.connect(os.environ["PACGO_QEMU_CONSOLE"])
    console.setblocking(False)
    console_log_path = Path(os.environ["PACGO_QEMU_CONSOLE_LOG"])
    try:
        deadline = time.monotonic() + wait_seconds
        with console_log_path.open("ab") as console_log:
            if wait_for_nonblack:
                if not screenshot:
                    raise ValueError(
                        "XFCE_IDLE_WAIT_FOR_NONBLACK requires XFCE_IDLE_SCREENSHOT"
                    )
                probe_path = Path(screenshot).resolve().with_suffix(".probe.ppm")
                probe_path.parent.mkdir(parents=True, exist_ok=True)
                # The wedge is recognised by the diagnostic serial stream going
                # quiet, and the host already has that stream -- so detection
                # stays here rather than in the kernel.  The one-shot read that
                # follows costs a handful of QMP commands; polling the guest
                # continuously perturbed startup badly enough to hide the bug.
                quiet_log = os.environ.get("XFCE_IDLE_QUIET_LOG", "")
                quiet_seconds = float(os.environ.get("XFCE_IDLE_QUIET_SECONDS", "20"))
                quiet_entries = [
                    e
                    for e in os.environ.get("XFCE_IDLE_WATCH_SYMBOLS", "").split(",")
                    if "=" in e
                ]
                # The same ping-pong runs in the startup stall on every boot,
                # so a couple of one-shot reads at a fixed elapsed time get the
                # data without waiting for the rare wedge.
                sample_at = [
                    float(v)
                    for v in os.environ.get("XFCE_IDLE_SAMPLE_AT_S", "").split(",")
                    if v.strip()
                ]
                quiet_path = Path(quiet_log) if quiet_log else None
                last_size = -1
                last_growth = time.monotonic()
                quiet_reported = False
                try:
                    while time.monotonic() < deadline:
                        probe_deadline = min(
                            deadline, time.monotonic() + probe_interval_seconds
                        )
                        drain_console(console, console_log, probe_deadline)
                        if sample_at and quiet_entries:
                            elapsed = (time.monotonic_ns() - started_ns) / 1e9
                            while sample_at and elapsed >= sample_at[0]:
                                sample_at.pop(0)
                                out = Path(screenshot).resolve().with_suffix(
                                    ".quiet.txt"
                                )
                                with out.open("a") as handle:
                                    handle.write(f"=== sample at {elapsed:.2f}s ===\n")
                                    handle.write(
                                        read_watch_symbols(qmp, quiet_entries) + "\n"
                                    )
                        if quiet_path is not None and quiet_entries:
                            size = (
                                quiet_path.stat().st_size
                                if quiet_path.exists()
                                else -1
                            )
                            if size != last_size:
                                last_size = size
                                last_growth = time.monotonic()
                                quiet_reported = False
                            elif (
                                not quiet_reported
                                and time.monotonic() - last_growth >= quiet_seconds
                            ):
                                quiet_reported = True
                                elapsed = (time.monotonic_ns() - started_ns) / 1e9
                                out = Path(screenshot).resolve().with_suffix(
                                    ".quiet.txt"
                                )
                                with out.open("a") as handle:
                                    handle.write(
                                        f"=== console quiet {quiet_seconds:.0f}s "
                                        f"at {elapsed:.2f}s ===\n"
                                    )
                                    # Three samples two seconds apart: the
                                    # question is which principal's counter
                                    # stands still, and one gap cannot tell a
                                    # stalled principal from an unlucky read.
                                    for _ in range(3):
                                        handle.write(
                                            read_watch_symbols(qmp, quiet_entries)
                                            + "\n"
                                        )
                                        time.sleep(2.0)
                                    handle.flush()
                        probe_path.unlink(missing_ok=True)
                        qmp.execute(
                            "screendump",
                            {
                                "filename": str(probe_path),
                                "format": "ppm",
                                "device": "pachagpu",
                            },
                        )
                        visible_nonblack_pixels = ppm_nonblack_pixels(probe_path)
                        if visible_nonblack_pixels >= minimum_nonblack_pixels:
                            visible_frame_ms = round(
                                (time.monotonic_ns() - started_ns) / 1_000_000, 3
                            )
                            drain_console(
                                console,
                                console_log,
                                min(deadline, time.monotonic() + settle_seconds),
                            )
                            break
                finally:
                    probe_path.unlink(missing_ok=True)
            else:
                drain_console(console, console_log, deadline)
        if action == "terminal":
            qmp.chord("ctrl", "alt", "t")
            time.sleep(action_wait_seconds)
        elif action:
            raise ValueError(f"invalid XFCE_IDLE_ACTION={action!r}")
        if cpu_dump_path:
            dump = []
            # A wedged guest and an idle guest both park every CPU in HLT.  The
            # kernel's 1 ms tick counter separates them: if it stands still the
            # BSP timer itself stopped, which also means any in-kernel idle
            # watchdog can never run (it only executes after HLT returns).
            # Comma-separated "name=0xaddr" kernel globals, sampled a second
            # apart.  Whether the scheduler is still placing threads separates
            # a total stall from a livelock that keeps rescheduling without
            # ever completing work; both look identical from outside.
            watch = os.environ.get("XFCE_IDLE_WATCH_SYMBOLS", "")
            entries = [e for e in watch.split(",") if "=" in e]
            if entries:
                lines = []
                for _ in range(3):
                    row = []
                    for entry in entries:
                        name, _, addr = entry.partition("=")
                        count, _, base = addr.partition("*")
                        if base:
                            words, addr = count, base
                        else:
                            words = "1"
                        raw = qmp.execute(
                            "human-monitor-command",
                            {
                                "command-line": f"x /{words}gx {addr}",
                                "cpu-index": 0,
                            },
                        ).strip()
                        if words == "1":
                            row.append(f"{name}={raw.split(':')[-1].strip()}")
                        else:
                            row.append(f"{name}=[{raw}]")
                    lines.append("  ".join(row))
                    time.sleep(1.0)
                dump.append(
                    "=== kernel globals (1 s apart) ===\n"
                    + "\n".join(lines)
                    + "\n\n"
                )
            # Whether each CPU's LAPIC timer is still armed, independent of any
            # kernel symbol: a stopped timer never returns from HLT, so the
            # in-kernel idle watchdog cannot report that class of wedge.
            lapic = []
            for cpu in range(4):
                lapic.append(
                    f"--- CPU {cpu} ---\n"
                    + qmp.execute(
                        "human-monitor-command",
                        {"command-line": "info lapic", "cpu-index": cpu},
                    )
                )
            dump.append("=== info lapic ===\n" + "".join(lapic) + "\n")
            dump.append(
                qmp.execute(
                    "human-monitor-command",
                    {"command-line": "info cpus"},
                )
            )
            for cpu in range(4):
                registers = qmp.execute(
                    "human-monitor-command",
                    {"command-line": "info registers", "cpu-index": cpu},
                )
                dump.append(f"\n=== CPU {cpu} ===\n{registers}")
            path = Path(cpu_dump_path)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("".join(dump))
        if screenshot:
            path = Path(screenshot).resolve()
            path.parent.mkdir(parents=True, exist_ok=True)
            qmp.execute(
                "screendump",
                {
                    "filename": str(path),
                    "format": screenshot_format,
                    "device": "pachagpu",
                },
            )
    finally:
        console.close()
        qmp.close()

    result = {
        "cpus": 4,
        "action": action,
        "host_wait_ms": round((time.monotonic_ns() - started_ns) / 1_000_000, 3),
        "screenshot": screenshot,
        "screenshot_format": screenshot_format,
        "wait_for_nonblack": wait_for_nonblack,
        "visible_frame_ms": visible_frame_ms,
        "visible_nonblack_pixels": visible_nonblack_pixels,
    }
    if result_path:
        path = Path(result_path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(result, sort_keys=True) + "\n")
    print("XFCE_IDLE_PROBE_DONE " + json.dumps(result, sort_keys=True), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
