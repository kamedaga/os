#!/usr/bin/env python3
"""Measure key-to-visible startup phases for Sway-launched GUI applications."""

from __future__ import annotations

import hashlib
import json
import os
import re
import select
import shutil
import socket
import sys
import threading
import time
from pathlib import Path
from typing import Callable


PROMPT = b"bash-5.3# "
PID_RE = re.compile(r" pid=(\d+)")


class QMP:
    def __init__(self, path: str) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5.0)
        self.sock.connect(path)
        self.stream = self.sock.makefile("rwb", buffering=0)
        self.rx_text = ""
        self.lock = threading.Lock()
        greeting = self._read_message()
        if "QMP" not in greeting:
            raise RuntimeError("invalid QMP greeting")
        self.execute("qmp_capabilities")

    def _read_message(self) -> dict:
        decoder = json.JSONDecoder()
        while True:
            candidate = self.rx_text.lstrip()
            if candidate:
                try:
                    value, end = decoder.raw_decode(candidate)
                except json.JSONDecodeError:
                    pass
                else:
                    self.rx_text = candidate[end:]
                    if not isinstance(value, dict):
                        raise RuntimeError(f"non-object QMP message: {value!r}")
                    return value
            line = self.stream.readline()
            if not line:
                raise EOFError("QMP socket closed")
            self.rx_text += line.decode("utf-8", errors="strict")

    def close(self) -> None:
        self.stream.close()
        self.sock.close()

    def execute(self, name: str, arguments: dict | None = None) -> object:
        with self.lock:
            request: dict[str, object] = {"execute": name}
            if arguments is not None:
                request["arguments"] = arguments
            self.stream.write(json.dumps(request).encode() + b"\r\n")
            while True:
                response = self._read_message()
                if "event" in response:
                    continue
                if "error" in response:
                    raise RuntimeError(f"QMP {name} failed: {response['error']}")
                if "return" not in response:
                    raise RuntimeError(f"QMP {name} returned no result: {response!r}")
                return response["return"]

    def key(self, code: str) -> None:
        events = [
            {
                "type": "key",
                "data": {
                    "down": True,
                    "key": {"type": "qcode", "data": code},
                },
            },
            {
                "type": "key",
                "data": {
                    "down": False,
                    "key": {"type": "qcode", "data": code},
                },
            },
        ]
        self.execute("input-send-event", {"events": events})

    def screendump(self, path: Path) -> None:
        path.unlink(missing_ok=True)
        self.execute(
            "screendump",
            {"filename": str(path.resolve()), "format": "ppm", "device": "pachagpu"},
        )


def ppm_digest(path: Path) -> str:
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
    if data[cursor : cursor + 2] == b"\r\n":
        cursor += 2
    else:
        cursor += 1
    width, height, maximum = map(int, tokens)
    if width <= 0 or height <= 0 or maximum != 255:
        raise ValueError(f"unsupported PPM geometry: {tokens!r}")
    pixels = data[cursor:]
    if len(pixels) != width * height * 3:
        raise ValueError(f"truncated PPM: {path}")
    return hashlib.sha256(pixels).hexdigest()


class ConsoleLines:
    def __init__(self, sock: socket.socket, live_log) -> None:
        self.sock = sock
        self.live_log = live_log
        self.buffer = bytearray()
        self.lines: list[tuple[int, str]] = []

    def wait_prompt(self, deadline: float) -> None:
        while time.monotonic() < deadline:
            if PROMPT in self.buffer:
                return
            self._receive(deadline)
        raise TimeoutError("guest prompt timeout")

    def _receive(self, deadline: float) -> None:
        timeout = max(0.0, min(0.1, deadline - time.monotonic()))
        readable, _, _ = select.select([self.sock], [], [], timeout)
        if not readable:
            return
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("virtio console closed")
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        self.live_log.write(chunk)
        self.live_log.flush()
        self.buffer.extend(chunk)
        while b"\n" in self.buffer:
            raw, _, remainder = self.buffer.partition(b"\n")
            self.buffer = bytearray(remainder)
            self.lines.append(
                (time.perf_counter_ns(), raw.decode(errors="replace").rstrip("\r"))
            )

    def wait_line(
        self,
        predicate: Callable[[str], bool],
        deadline: float,
        start: int = 0,
    ) -> tuple[int, int, str]:
        cursor = start
        while time.monotonic() < deadline:
            while cursor < len(self.lines):
                timestamp_ns, line = self.lines[cursor]
                cursor += 1
                if "KEY_PHASE_FAIL" in line:
                    raise RuntimeError(line)
                if predicate(line):
                    return cursor, timestamp_ns, line
            self._receive(deadline)
        tail = "\n".join(line for _, line in self.lines[-40:])
        raise TimeoutError(f"console marker timeout; tail={tail}")

    def find_line(
        self,
        predicate: Callable[[str], bool],
        start: int,
        end: int | None = None,
    ) -> tuple[int, int, str] | None:
        stop = len(self.lines) if end is None else min(end, len(self.lines))
        for index in range(start, stop):
            timestamp_ns, line = self.lines[index]
            if predicate(line):
                return index + 1, timestamp_ns, line
        return None


class ScreenProbe(threading.Thread):
    def __init__(
        self,
        qmp: QMP,
        out_dir: Path,
        app: str,
        baseline_digest: str,
    ) -> None:
        super().__init__(daemon=True)
        self.qmp = qmp
        self.out_dir = out_dir
        self.app = app
        self.baseline_digest = baseline_digest
        self.first_change_ns: int | None = None
        self.stable_ns: int | None = None
        self.polls = 0
        self.error: Exception | None = None
        self.ready = threading.Event()
        self.start_capture = threading.Event()
        self.stop_requested = threading.Event()

    def run(self) -> None:
        try:
            self.ready.set()
            if not self.start_capture.wait(5.0):
                raise TimeoutError("screen probe start timeout")
            last_digest = self.baseline_digest
            unchanged = 0
            first_path = self.out_dir / f"{self.app}-first-visible.ppm"
            stable_path = self.out_dir / f"{self.app}-stable.ppm"
            deadline = time.monotonic() + 30.0
            while time.monotonic() < deadline and not self.stop_requested.is_set():
                path = self.out_dir / f".{self.app}-poll.ppm"
                self.qmp.screendump(path)
                captured_ns = time.perf_counter_ns()
                digest = ppm_digest(path)
                self.polls += 1
                if self.first_change_ns is None:
                    if digest != self.baseline_digest:
                        self.first_change_ns = captured_ns
                        shutil.copyfile(path, first_path)
                        last_digest = digest
                        unchanged = 0
                elif digest == last_digest:
                    unchanged += 1
                    if unchanged >= 5:
                        self.stable_ns = captured_ns
                        shutil.copyfile(path, stable_path)
                        break
                else:
                    last_digest = digest
                    unchanged = 0
                time.sleep(0.005)
            (self.out_dir / f".{self.app}-poll.ppm").unlink(missing_ok=True)
        except Exception as exc:  # diagnostic path: preserve the exact failure
            self.error = exc
            self.ready.set()


def phase_ms(end: int, start: int) -> float:
    return round((end - start) / 1_000_000.0, 3)


def stable_baseline(qmp: QMP, out_dir: Path, app: str) -> tuple[Path, str]:
    path = out_dir / f"{app}-baseline.ppm"
    previous = ""
    stable = 0
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        qmp.screendump(path)
        digest = ppm_digest(path)
        if digest == previous:
            stable += 1
            if stable >= 3:
                return path, digest
        else:
            previous = digest
            stable = 0
        time.sleep(0.02)
    raise TimeoutError(f"framebuffer did not settle before {app}")


def main() -> int:
    timeout = float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "120"))
    deadline = time.monotonic() + timeout
    qmp_path = os.environ["KEY_PHASE_QMP_SOCKET"]
    out_dir = Path(os.environ["KEY_PHASE_OUT_DIR"])
    result_path = Path(os.environ["KEY_PHASE_RESULT"])
    live_log_path = Path(os.environ["KEY_PHASE_LIVE_LOG"])
    known_apps = {
        "foot": ("f1", "f3"),
        "thunar": ("f2", "f4"),
    }
    requested_apps = os.environ.get("KEY_PHASE_APPS", "foot thunar").split()
    if not requested_apps or any(app not in known_apps for app in requested_apps):
        raise ValueError(f"invalid KEY_PHASE_APPS: {requested_apps!r}")
    window_only_apps = set(os.environ.get("KEY_PHASE_WINDOW_ONLY_APPS", "").split())
    if any(app not in requested_apps for app in window_only_apps):
        raise ValueError(
            f"invalid KEY_PHASE_WINDOW_ONLY_APPS: {sorted(window_only_apps)!r}"
        )
    out_dir.mkdir(parents=True, exist_ok=True)

    console_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    qmp: QMP | None = None
    live_log = live_log_path.open("wb")
    try:
        console_socket.connect(os.environ["PACGO_QEMU_CONSOLE"])
        console_socket.setblocking(False)
        console = ConsoleLines(console_socket, live_log)
        # The initial prompt can race ahead of the host chardev connection.
        # A newline requests a fresh prompt after this client is attached.
        console_socket.sendall(b"\n")
        console.wait_prompt(deadline)
        guest_apps = " ".join(requested_apps)
        dump_app_logs = os.environ.get("KEY_PHASE_DUMP_APP_LOGS", "0")
        apk_add_gedit = os.environ.get("KEY_PHASE_APK_ADD_GEDIT", "0")
        no_at_bridge = os.environ.get("KEY_PHASE_NO_AT_BRIDGE", "0")
        glycin_trace = os.environ.get("KEY_PHASE_GLYCIN_TRACE", "0")
        rayon_threads = os.environ.get("KEY_PHASE_RAYON_THREADS", "")
        blocking_max_threads = os.environ.get(
            "KEY_PHASE_BLOCKING_MAX_THREADS", ""
        )
        session_tmpdir = os.environ.get("KEY_PHASE_TMPDIR", "")
        glycin_rust_log = os.environ.get("KEY_PHASE_GLYCIN_RUST_LOG", "")
        if apk_add_gedit not in {"0", "1"}:
            raise ValueError(f"invalid KEY_PHASE_APK_ADD_GEDIT={apk_add_gedit!r}")
        if no_at_bridge not in {"0", "1"}:
            raise ValueError(f"invalid KEY_PHASE_NO_AT_BRIDGE={no_at_bridge!r}")
        if glycin_trace not in {"0", "1"}:
            raise ValueError(f"invalid KEY_PHASE_GLYCIN_TRACE={glycin_trace!r}")
        if rayon_threads and (
            not rayon_threads.isdigit() or not 1 <= int(rayon_threads) <= 64
        ):
            raise ValueError(
                f"invalid KEY_PHASE_RAYON_THREADS={rayon_threads!r}"
            )
        if blocking_max_threads and (
            not blocking_max_threads.isdigit()
            or not 1 <= int(blocking_max_threads) <= 64
        ):
            raise ValueError(
                "invalid KEY_PHASE_BLOCKING_MAX_THREADS="
                f"{blocking_max_threads!r}"
            )
        if session_tmpdir not in {"", "/run/user/0"}:
            raise ValueError(f"invalid KEY_PHASE_TMPDIR={session_tmpdir!r}")
        if glycin_rust_log not in {"", "glycin=trace"}:
            raise ValueError(
                f"invalid KEY_PHASE_GLYCIN_RUST_LOG={glycin_rust_log!r}"
            )
        rayon_environment = (
            f"RAYON_NUM_THREADS='{rayon_threads}' " if rayon_threads else ""
        )
        blocking_environment = (
            f"BLOCKING_MAX_THREADS='{blocking_max_threads}' "
            if blocking_max_threads else ""
        )
        tmpdir_environment = (
            f"TMPDIR='{session_tmpdir}' " if session_tmpdir else ""
        )
        rust_log_environment = (
            f"RUST_LOG='{glycin_rust_log}' " if glycin_rust_log else ""
        )
        console_socket.sendall(
            (
                rayon_environment +
                blocking_environment +
                tmpdir_environment +
                rust_log_environment +
                f"KEY_PHASE_APPS='{guest_apps}' "
                f"KEY_PHASE_DUMP_APP_LOGS='{dump_app_logs}' "
                f"KEY_PHASE_APK_ADD_GEDIT='{apk_add_gedit}' "
                f"KEY_PHASE_NO_AT_BRIDGE='{no_at_bridge}' "
                f"KEY_PHASE_GLYCIN_TRACE='{glycin_trace}' "
                "bash /cmd/key_launch_phase_benchmark.sh\n"
            ).encode()
        )

        cursor, _, calibration_line = console.wait_line(
            lambda line: "KEY_PHASE_TSC_CALIBRATION" in line, deadline
        )
        hz_match = re.search(r" hz=(\d+)", calibration_line)
        if hz_match is None:
            raise RuntimeError(f"invalid TSC calibration: {calibration_line}")
        tsc_hz = int(hz_match.group(1))

        qmp = QMP(qmp_path)
        results: dict[str, object] = {
            "phase_clock": "host_perf_counter_ns_at_virtio_console_receive",
            "tsc_hz": tsc_hz,
            "apk_add_gedit": apk_add_gedit == "1",
            "no_at_bridge": no_at_bridge == "1",
            "rayon_threads": int(rayon_threads) if rayon_threads else None,
            "blocking_max_threads": (
                int(blocking_max_threads) if blocking_max_threads else None
            ),
            "tmpdir": session_tmpdir or None,
            "apps": {},
        }

        for app in requested_apps:
            launch_key, close_key = known_apps[app]
            cursor, ready_host_ns, ready_line = console.wait_line(
                lambda line, app=app: f"KEY_PHASE_READY app={app} " in line,
                deadline,
                cursor,
            )
            _, baseline_digest = stable_baseline(qmp, out_dir, app)

            input_start_ns = time.perf_counter_ns()
            qmp.key(launch_key)
            input_ack_ns = time.perf_counter_ns()
            phase_cursor = cursor
            cursor, process_host_ns, process_line = console.wait_line(
                lambda line, app=app: (
                    f"KEY_PHASE_PROBE app={app} stage=process_created " in line
                ),
                deadline,
                cursor,
            )
            pid_match = PID_RE.search(process_line)
            if pid_match is None:
                raise RuntimeError(f"missing process pid: {process_line}")
            pid = int(pid_match.group(1))

            binding_predicate = lambda line, app=app: (
                "KEY_PHASE_SWAY_EVENT" in line
                and f"lpr_key_launch_probe.elf launch {app}" in line
            )
            launcher_predicate = lambda line, app=app: (
                f"KEY_PHASE_PROBE app={app} stage=launcher_start " in line
            )
            binding_observation = console.find_line(
                binding_predicate, phase_cursor, cursor
            )
            launcher_observation = console.find_line(
                launcher_predicate, phase_cursor, cursor
            )
            while binding_observation is None or launcher_observation is None:
                cursor, observed_ns, observed_line = console.wait_line(
                    lambda line: binding_predicate(line) or launcher_predicate(line),
                    deadline,
                    cursor,
                )
                if binding_observation is None and binding_predicate(observed_line):
                    binding_observation = (cursor, observed_ns, observed_line)
                if launcher_observation is None and launcher_predicate(observed_line):
                    launcher_observation = (cursor, observed_ns, observed_line)
            _, binding_host_ns, binding_line = binding_observation
            _, launcher_host_ns, launcher_line = launcher_observation

            window_predicate = lambda line, pid=pid: (
                    "KEY_PHASE_SWAY_EVENT" in line
                    and ('\"change\": \"new\"' in line or '\"change\":\"new\"' in line)
                    and re.search(rf'\"pid\"\s*:\s*{pid}(?:[,}}])', line) is not None
            )
            exit_predicate = lambda line, app=app, pid=pid: (
                f"KEY_PHASE_PROBE app={app} stage=process_exit " in line
                and f" pid={pid} " in f" {line} "
            )
            window_observation = console.find_line(
                window_predicate, phase_cursor, cursor
            )
            if window_observation is None:
                cursor, window_host_ns, window_line = console.wait_line(
                    lambda line: window_predicate(line) or exit_predicate(line),
                    deadline,
                    cursor,
                )
                if exit_predicate(window_line):
                    # The guest emits captured stderr immediately after the
                    # exit marker.  Consume it before failing so a crashed
                    # application does not turn into an opaque full-timeout.
                    try:
                        cursor, _, _ = console.wait_line(
                            lambda line, app=app: (
                                f"KEY_PHASE_APP_DONE app={app} " in line
                            ),
                            min(deadline, time.monotonic() + 2.0),
                            cursor,
                        )
                    except TimeoutError:
                        pass
                    raise RuntimeError(
                        f"{app} exited before creating a window: {window_line}"
                    )
            else:
                _, window_host_ns, window_line = window_observation

            screen: ScreenProbe | None = None
            if app not in window_only_apps:
                # Full 1024x768 screendumps take the QEMU display lock and
                # write 2.3 MiB each. Polling from key-down materially slows
                # startup, so begin only after Sway reports the mapped window.
                screen = ScreenProbe(qmp, out_dir, app, baseline_digest)
                screen.start()
                if not screen.ready.wait(5.0):
                    raise TimeoutError("screen probe QMP connection timeout")
                if screen.error is not None:
                    raise screen.error
                screen.start_capture.set()

                screen.join(timeout=max(0.1, deadline - time.monotonic()))
                if screen.error is not None:
                    raise screen.error
                if screen.first_change_ns is None:
                    raise TimeoutError(f"no framebuffer change for {app}")
                if screen.stable_ns is None:
                    raise TimeoutError(f"framebuffer did not stabilize for {app}")

            # PachaOS currently exposes EPOCHREALTIME to bash at whole-second
            # resolution. Timestamp all phases at the common virtio-console
            # receiver; the TSC profile covers internal work separately.
            binding_ns = binding_host_ns
            launcher_ns = launcher_host_ns
            process_ns = process_host_ns
            window_ns = window_host_ns
            timestamps_ns = {
                "input_start": input_start_ns,
                "input_ack": input_ack_ns,
                "binding": binding_ns,
                "launcher": launcher_ns,
                "process_created": process_ns,
                "window_new": window_ns,
            }
            phases = {
                "qmp_input_ack": phase_ms(input_ack_ns, input_start_ns),
                "key_to_binding": phase_ms(binding_ns, input_start_ns),
                "binding_to_launcher": phase_ms(launcher_ns, binding_ns),
                "launcher_to_process": phase_ms(process_ns, launcher_ns),
                "process_to_window": phase_ms(window_ns, process_ns),
                "key_to_window": phase_ms(window_ns, input_start_ns),
            }
            if screen is not None:
                assert screen.first_change_ns is not None
                assert screen.stable_ns is not None
                timestamps_ns.update(
                    {
                        "first_visible": screen.first_change_ns,
                        "display_stable": screen.stable_ns,
                    }
                )
                phases.update(
                    {
                        "window_to_first_visible": phase_ms(screen.first_change_ns, window_ns),
                        "first_visible_to_stable": phase_ms(
                            screen.stable_ns, screen.first_change_ns
                        ),
                        "key_to_first_visible": phase_ms(
                            screen.first_change_ns, input_start_ns
                        ),
                        "key_to_display_stable": phase_ms(
                            screen.stable_ns, input_start_ns
                        ),
                    }
                )
            app_result = {
                "pid": pid,
                "key": launch_key,
                "screen_probe": "skipped" if screen is None else "captured",
                "polls": 0 if screen is None else screen.polls,
                "timestamps_ns": timestamps_ns,
                "host_observed_ns": {
                    "ready": ready_host_ns,
                    "binding": binding_host_ns,
                    "launcher": launcher_host_ns,
                    "process_created": process_host_ns,
                    "window_new": window_host_ns,
                },
                "phase_ms": phases,
            }
            results["apps"][app] = app_result  # type: ignore[index]
            print(
                "KEY_PHASE_HOST_RESULT "
                + json.dumps({"app": app, **app_result}, sort_keys=True),
                flush=True,
            )

            qmp.key(close_key)
            cursor, _, _ = console.wait_line(
                lambda line, app=app: (
                    f"KEY_PHASE_PROBE app={app} stage=process_exit " in line
                ),
                deadline,
                cursor,
            )

        console.wait_line(
            lambda line: "KEY_PHASE_COMPLETE" in line, deadline, cursor
        )
        result_path.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n")
        print("KEY_PHASE_BENCHMARK=OK", flush=True)
        return 0
    except Exception as exc:
        print(f"KEY_PHASE_BENCHMARK=FAIL error={exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        if qmp is not None:
            qmp.close()
        console_socket.close()
        live_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
