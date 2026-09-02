#!/usr/bin/env python3
"""Measure post-gedit GUI startup without modifying the guest disk image."""

from __future__ import annotations

import json
import os
import select
import socket
import sys
import time
from dataclasses import dataclass
from pathlib import Path


PROMPT = b"bash-5.3# "
SAMPLE_INTERVAL_NS = 20_000_000
SERIAL_COUNTER_NEEDLES = {
    "vtd_checkpoints": b"vtd: runtime checkpoint",
    "vtd_fault_scans": b"vtd: faults count=",
    "unix_closes": b"[netd] unix_close",
    "unix_orphan_reaps": b"[netd] unix_orphan_reap",
    "exec_self_misses": b"[filed] exec_self open failed",
    "mmap_failures": b"[lpr] mmap failure",
}

GUEST_SCRIPT = r'''#!/bin/bash
set -u

runtime=/run/user/0
config=/cmd/phase4_gui_benchmark.conf
target=${1:?missing-target}
wait_limit=600
event_log=/tmp/gui-startup-window-events.log
app_log=/tmp/gui-startup-app.log

fail()
{
    printf 'GUI_STARTUP_FAIL stage=%s target=%s\n' "$1" "$target"
    exit 1
}

wait_pipe_start()
{
    coproc GUI_WAIT_PIPE { while IFS= read -r _; do :; done; }
    wait_fd=${GUI_WAIT_PIPE[0]}
    wait_pid=$GUI_WAIT_PIPE_PID
}

wait_tick()
{
    IFS= read -r -t "$1" -u "$wait_fd" _ || true
}

cleanup()
{
    if [[ ${monitor_pid:-0} -gt 0 ]]; then
        kill -KILL "$monitor_pid" 2>/dev/null || true
    fi
    if [[ ${app_pid:-0} -gt 0 ]]; then
        kill -KILL "$app_pid" 2>/dev/null || true
    fi
    if [[ ${sway_pid:-0} -gt 0 ]]; then
        kill -KILL "$sway_pid" 2>/dev/null || true
    fi
    if [[ ${wait_pid:-0} -gt 0 ]]; then
        kill -KILL "$wait_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
wait_pipe_start

if ! apk info -e gedit >/dev/null 2>&1; then
    fail gedit-not-installed
fi
gedit_version=$(apk info -v gedit 2>/dev/null | head -n 1)
[[ -n $gedit_version ]] || gedit_version=gedit
[[ -x /usr/bin/gedit ]] || fail gedit-executable
printf 'GUI_STARTUP_PRECHECK package=%s installed=1 target=%s\n' \
    "$gedit_version" "$target"

case "$target" in
glycin-app-png)
    /bin/cp /usr/bin/gdk-pixbuf-thumbnailer /tmp/glycin-app-thumbnailer || \
        fail glycin-app-probe-copy
    /bin/rm -f /tmp/gui-startup-wallpaper.png
    printf 'GUI_STARTUP_EXEC app=glycin-app-png input=sway-wallpaper\n'
    /tmp/glycin-app-thumbnailer \
        /usr/share/backgrounds/sway/Sway_Wallpaper_Blue_1920x1080.png \
        /tmp/gui-startup-wallpaper.png >"$app_log" 2>&1 || {
        /bin/cat "$app_log"
        fail glycin-app-png-exec
    }
    [[ -s /tmp/gui-startup-wallpaper.png ]] || fail glycin-app-png-output
    printf 'GUI_STARTUP_READY app=glycin-app-png output_bytes=%s\n' \
        "$(stat -c %s /tmp/gui-startup-wallpaper.png)"
    exit 0
    ;;
glycin-png)
    /bin/rm -f /tmp/gui-startup-wallpaper.png
    printf 'GUI_STARTUP_EXEC app=glycin-png input=sway-wallpaper\n'
    /usr/bin/gdk-pixbuf-thumbnailer \
        /usr/share/backgrounds/sway/Sway_Wallpaper_Blue_1920x1080.png \
        /tmp/gui-startup-wallpaper.png >"$app_log" 2>&1 || {
        /bin/cat "$app_log"
        fail glycin-png-exec
    }
    [[ -s /tmp/gui-startup-wallpaper.png ]] || fail glycin-png-output
    printf 'GUI_STARTUP_READY app=glycin-png output_bytes=%s\n' \
        "$(stat -c %s /tmp/gui-startup-wallpaper.png)"
    exit 0
    ;;
glycin-svg)
    /bin/rm -f /tmp/gui-startup-image-missing.png
    printf 'GUI_STARTUP_EXEC app=glycin-svg input=adwaita-image-missing\n'
    /usr/bin/gdk-pixbuf-thumbnailer \
        /usr/share/icons/Adwaita/scalable/status/image-missing.svg \
        /tmp/gui-startup-image-missing.png >"$app_log" 2>&1 || {
        /bin/cat "$app_log"
        fail glycin-svg-exec
    }
    [[ -s /tmp/gui-startup-image-missing.png ]] || fail glycin-svg-output
    printf 'GUI_STARTUP_READY app=glycin-svg output_bytes=%s\n' \
        "$(stat -c %s /tmp/gui-startup-image-missing.png)"
    exit 0
    ;;
sway|foot|gedit) ;;
*) fail invalid-target ;;
esac

unset WAYLAND_DISPLAY SWAYSOCK
printf 'GUI_STARTUP_EXEC app=sway target=%s\n' "$target"
/usr/bin/sway -c "$config" > /tmp/gui-startup-sway.log 2>&1 &
sway_pid=$!

socket=
display=
ticks=0
while [[ $ticks -lt $wait_limit ]]; do
    shopt -s nullglob
    sockets=("$runtime"/sway-ipc.*.sock)
    displays=("$runtime"/wayland-*)
    shopt -u nullglob
    if [[ ${#sockets[@]} -eq 1 ]]; then
        socket=${sockets[0]}
    fi
    for path in "${displays[@]}"; do
        [[ $path == *.lock ]] && continue
        display=${path##*/}
        break
    done
    [[ -n $socket && -n $display ]] && break
    kill -0 "$sway_pid" 2>/dev/null || {
        /bin/cat /tmp/gui-startup-sway.log
        fail sway-exited
    }
    wait_tick 0.05
    ticks=$((ticks + 1))
done
[[ -n $socket ]] || fail sway-socket-timeout
[[ -n $display ]] || fail wayland-display-timeout
export SWAYSOCK=$socket WAYLAND_DISPLAY=$display

P4_BACKGROUND_PROBE=1 /cmd/lpr_wayland_animation_bench.elf \
    >/tmp/gui-startup-background.log 2>&1 || {
    /bin/cat /tmp/gui-startup-background.log
    fail sway-presentation
}

/bin/rm -f "$event_log"
/cmd/lpr_sway_event_monitor.elf "$socket" >"$event_log" 2>&1 &
monitor_pid=$!
ticks=0
while ! /bin/grep -Eq '"first"[[:space:]]*:[[:space:]]*true' \
    "$event_log" 2>/dev/null; do
    kill -0 "$monitor_pid" 2>/dev/null || fail sway-event-monitor
    [[ $ticks -lt $wait_limit ]] || fail sway-event-timeout
    wait_tick 0.05
    ticks=$((ticks + 1))
done
printf 'GUI_STARTUP_READY app=sway target=%s display=%s\n' "$target" "$display"
[[ $target != sway ]] || exit 0

printf 'GUI_STARTUP_EXEC app=%s\n' "$target"
printf 'GUI_STARTUP_APP_CONTEXT app=%s display=%s socket=%s socket_exists=%s\n' \
    "$target" "$WAYLAND_DISPLAY" "$SWAYSOCK" "$([[ -S $runtime/$WAYLAND_DISPLAY ]] && echo 1 || echo 0)"
if [[ $target == foot ]]; then
    /usr/bin/foot /bin/sh -c 'exec /bin/sleep 60' >"$app_log" 2>&1 &
else
    GDK_BACKEND=wayland /usr/bin/gedit --new-window >"$app_log" 2>&1 &
fi
app_pid=$!
pid_token="\"pid\": $app_pid"

ticks=0
window_event=
while [[ $ticks -lt $wait_limit ]]; do
    while IFS= read -r line; do
        [[ $line == *'"change": "new"'* ]] || continue
        if [[ $line == *"$pid_token"* ]]; then
            window_event=$line
            break 2
        fi
        if [[ $target == foot && $line == *'"app_id": "foot"'* ]]; then
            window_event=$line
            break 2
        fi
        if [[ $target == gedit && $line == *'"app_id": "gedit"'* ]]; then
            window_event=$line
            break 2
        fi
    done <"$event_log"
    wait_tick 0.05
    ticks=$((ticks + 1))
done
if [[ -z $window_event ]]; then
    /bin/cat "$app_log"
    /bin/cat "$event_log"
    fail app-window-timeout
fi
printf 'GUI_STARTUP_READY app=%s pid=%s\n' "$target" "$app_pid"
'''


@dataclass
class CpuSample:
    timestamp_ns: int
    ticks: dict[int, int]


class BenchmarkConsole:
    def __init__(self, sock: socket.socket, log_file) -> None:
        self.sock = sock
        self.log_file = log_file
        self.buffer = bytearray()
        self.samples: list[CpuSample] = []
        self.vcpu_tids: list[int] = []
        self.cpu_source = "unavailable"
        self.last_sample_ns = 0

    @staticmethod
    def _qmp_vcpu_tids(command: bytes) -> list[int]:
        arguments = [argument for argument in command.split(b"\0") if argument]
        qmp_path: str | None = None
        for index, argument in enumerate(arguments[:-1]):
            if argument != b"-qmp":
                continue
            endpoint = arguments[index + 1]
            if endpoint.startswith(b"unix:"):
                qmp_path = endpoint[5:].split(b",", 1)[0].decode()
                break
        if qmp_path is None:
            return []

        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(1.0)
        try:
            client.connect(qmp_path)
            stream = client.makefile("rwb", buffering=0)

            def receive_response() -> dict:
                while True:
                    line = stream.readline()
                    if not line:
                        raise EOFError("QMP socket closed")
                    response = json.loads(line)
                    if "return" in response or "error" in response:
                        return response

            greeting = json.loads(stream.readline())
            if "QMP" not in greeting:
                return []
            stream.write(b'{"execute":"qmp_capabilities"}\r\n')
            if "error" in receive_response():
                return []
            stream.write(b'{"execute":"query-cpus-fast"}\r\n')
            response = receive_response()
            return [
                int(cpu["thread-id"])
                for cpu in response.get("return", [])
                if "thread-id" in cpu
            ]
        except (OSError, ValueError, json.JSONDecodeError, EOFError):
            return []
        finally:
            client.close()

    def _discover_vcpus(self) -> None:
        path = Path(".artifacts/qemu-tty-vcpus.tsv")
        tids: list[int] = []
        if path.exists():
            for line in path.read_text(errors="replace").splitlines():
                fields = line.split("\t")
                if len(fields) != 2:
                    continue
                try:
                    tids.append(int(fields[1]))
                except ValueError:
                    continue
        if tids:
            self.vcpu_tids = tids
            self.cpu_source = "qmp-vcpu"
            return

        console_path = os.environ.get("PACGO_QEMU_CONSOLE", "").encode()
        if not console_path:
            return
        for proc in Path("/proc").iterdir():
            if not proc.name.isdigit():
                continue
            try:
                command = (proc / "cmdline").read_bytes()
            except (FileNotFoundError, PermissionError):
                continue
            if console_path not in command or b"qemu-system-x86_64" not in command:
                continue
            qmp_tids = self._qmp_vcpu_tids(command)
            if qmp_tids:
                self.vcpu_tids = qmp_tids
                self.cpu_source = "qmp-direct-vcpu"
                return
            tasks = proc / "task"
            all_tids: list[int] = []
            cpu_tids: list[int] = []
            try:
                entries = list(tasks.iterdir())
            except (FileNotFoundError, PermissionError):
                continue
            for entry in entries:
                if not entry.name.isdigit():
                    continue
                tid = int(entry.name)
                all_tids.append(tid)
                try:
                    name = (entry / "comm").read_text(errors="replace").strip()
                except (FileNotFoundError, PermissionError):
                    continue
                if name.startswith("CPU ") or "/KVM" in name:
                    cpu_tids.append(tid)
            self.vcpu_tids = cpu_tids or all_tids
            self.cpu_source = "proc-vcpu" if cpu_tids else "proc-qemu-all-threads"
            return

    @staticmethod
    def _thread_ticks(tid: int) -> int | None:
        try:
            fields = Path(f"/proc/{tid}/schedstat").read_text().split()
        except (FileNotFoundError, PermissionError):
            return None
        if not fields:
            return None
        return int(fields[0])

    def sample_cpu(self, force: bool = False) -> None:
        now = time.monotonic_ns()
        if not force and now - self.last_sample_ns < SAMPLE_INTERVAL_NS:
            return
        if not self.vcpu_tids:
            self._discover_vcpus()
        ticks: dict[int, int] = {}
        for tid in self.vcpu_tids:
            value = self._thread_ticks(tid)
            if value is not None:
                ticks[tid] = value
        if ticks:
            self.samples.append(CpuSample(now, ticks))
        self.last_sample_ns = now

    def receive(self, deadline: float) -> None:
        timeout = min(0.05, max(0.0, deadline - time.monotonic()))
        readable, _, _ = select.select([self.sock], [], [], timeout)
        self.sample_cpu()
        if not readable:
            return
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("virtio console closed")
        self.buffer.extend(chunk)
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        if self.log_file is not None:
            self.log_file.write(chunk)

    def wait_for(self, needle: bytes, deadline: float, start: int = 0) -> tuple[int, int]:
        while time.monotonic() < deadline:
            position = self.buffer.find(needle, start)
            if position >= 0:
                self.sample_cpu(force=True)
                return position, time.monotonic_ns()
            self.receive(deadline)
        raise AssertionError(
            f"missing {needle!r}; tail={bytes(self.buffer[-6000:])!r}"
        )

    def wait_for_any(
        self, needles: tuple[bytes, ...], deadline: float, start: int = 0
    ) -> tuple[bytes, int, int]:
        while time.monotonic() < deadline:
            matches = [
                (self.buffer.find(needle, start), needle) for needle in needles
            ]
            matches = [(position, needle) for position, needle in matches if position >= 0]
            if matches:
                position, needle = min(matches, key=lambda item: item[0])
                self.sample_cpu(force=True)
                return needle, position, time.monotonic_ns()
            self.receive(deadline)
        raise AssertionError(
            f"missing one of {needles!r}; tail={bytes(self.buffer[-6000:])!r}"
        )

    def command(
        self,
        command: bytes,
        marker: bytes,
        deadline: float,
        require_success: bool = True,
    ) -> bytes:
        start = len(self.buffer)
        self.sock.sendall(command + b"; printf '\\n" + marker + b"=%s\\n' \"$?\"\n")
        position, _ = self.wait_for(marker + b"=", deadline, start)
        self.wait_for(PROMPT, deadline, position)
        output = bytes(self.buffer[start:])
        if require_success and marker + b"=0" not in output:
            raise AssertionError(
                f"command failed marker={marker!r}; tail={output[-6000:]!r}"
            )
        return output

    def phase_cpu(self, start_ns: int, end_ns: int) -> dict[str, float | int | None]:
        selected = [
            sample for sample in self.samples
            if start_ns <= sample.timestamp_ns <= end_ns
        ]
        if len(selected) < 2:
            return {
                "vcpu_ms": None,
                "avg_vcpus": None,
                "peak_busy_vcpus": None,
                "cpu_source": self.cpu_source,
            }
        total_runtime_ns = 0
        peak_busy = 0
        for previous, current in zip(selected, selected[1:]):
            busy = 0
            for tid, value in current.ticks.items():
                delta = value - previous.ticks.get(tid, value)
                if delta > 0:
                    total_runtime_ns += delta
                    busy += 1
            peak_busy = max(peak_busy, busy)
        wall_ms = (end_ns - start_ns) / 1_000_000
        vcpu_ms = total_runtime_ns / 1_000_000
        return {
            "vcpu_ms": round(vcpu_ms, 3),
            "avg_vcpus": round(vcpu_ms / wall_ms, 3) if wall_ms else 0.0,
            "peak_busy_vcpus": peak_busy,
            "cpu_source": self.cpu_source,
        }

    @staticmethod
    def serial_counters() -> dict[str, int]:
        path = Path(".artifacts/serial-tty-test.log")
        try:
            data = path.read_bytes()
        except FileNotFoundError:
            data = b""
        return {
            name: data.count(needle) for name, needle in SERIAL_COUNTER_NEEDLES.items()
        }


def main() -> int:
    target = os.environ.get("GUI_STARTUP_TARGET", "gedit")
    if target not in {
        "sway", "foot", "gedit", "glycin-app-png", "glycin-app-png-3",
        "glycin-app-png-small-3", "glycin-png", "glycin-svg"
    }:
        print(f"GUI_STARTUP_HOST_FAIL invalid target={target!r}", file=sys.stderr)
        return 2
    timeout = float(os.environ.get("GUI_STARTUP_TIMEOUT_SECONDS", "90"))
    deadline = time.monotonic() + timeout
    result_path = os.environ.get("GUI_STARTUP_RESULT")
    console_log_path = os.environ.get("PACGO_QEMU_CONSOLE_LOG")
    console_log = (
        open(console_log_path, "ab", buffering=0) if console_log_path else None
    )
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    console = BenchmarkConsole(sock, console_log)
    try:
        sock.connect(os.environ["PACGO_QEMU_CONSOLE"])
        sock.setblocking(False)
        console.wait_for(PROMPT, deadline)

        installed = console.command(
            b"apk info -e gedit",
            b"GUI_GEDIT_PRESENT",
            deadline,
            require_success=False,
        )
        if b"GUI_GEDIT_PRESENT=0" not in installed:
            print("GUI_STARTUP_INSTALL_BEGIN package=gedit", flush=True)
            console.command(b"apk update", b"GUI_APK_UPDATE", deadline)
            console.command(b"apk add gedit", b"GUI_APK_ADD_GEDIT", deadline)
            print("GUI_STARTUP_INSTALL_READY package=gedit", flush=True)
        console.command(
            b"apk info -e gedit && test -x /usr/bin/gedit",
            b"GUI_GEDIT_INSTALLED",
            deadline,
        )

        start_offset = len(console.buffer)
        profile_prefix = (
            b"LPR_PROFILE_GRACEFUL=1 "
            if os.environ.get("GUI_STARTUP_GRACEFUL", "0") == "1"
            else b""
        )
        sock.sendall(
            profile_prefix
            + b"bash /cmd/gui_startup_benchmark.sh "
            + target.encode()
            + b"\n"
        )
        precheck_marker, precheck_position, _ = console.wait_for_any(
            (b"GUI_STARTUP_PRECHECK package=gedit", b"GUI_STARTUP_FAIL stage="),
            deadline,
            start_offset,
        )
        if precheck_marker.startswith(b"GUI_STARTUP_FAIL"):
            raise AssertionError(bytes(console.buffer[precheck_position:]).decode(errors="replace"))

        phases: dict[str, dict[str, float | int | None]] = {}
        phase_targets = [target] if target.startswith("glycin-") else ["sway"]
        if target in {"foot", "gedit"}:
            phase_targets.append(target)
        search_start = precheck_position
        for app in phase_targets:
            exec_position, exec_ns = console.wait_for(
                f"GUI_STARTUP_EXEC app={app}".encode(), deadline, search_start
            )
            serial_before = console.serial_counters()
            ready_marker, ready_position, ready_ns = console.wait_for_any(
                (
                    f"GUI_STARTUP_READY app={app}".encode(),
                    b"GUI_STARTUP_FAIL stage=",
                ),
                deadline,
                exec_position,
            )
            if ready_marker.startswith(b"GUI_STARTUP_FAIL"):
                raise AssertionError(
                    bytes(console.buffer[ready_position:]).decode(errors="replace")
                )
            wall_ms = (ready_ns - exec_ns) / 1_000_000
            phases[app] = {
                "wall_ms": round(wall_ms, 3),
                **console.phase_cpu(exec_ns, ready_ns),
                "serial_events": {
                    name: count - serial_before.get(name, 0)
                    for name, count in console.serial_counters().items()
                },
            }
            search_start = ready_position

        result = {
            "target": target,
            "gedit_precondition": "installed",
            "phases": phases,
        }
        rendered = json.dumps(result, sort_keys=True)
        if result_path:
            Path(result_path).parent.mkdir(parents=True, exist_ok=True)
            Path(result_path).write_text(rendered + "\n")
        print(f"\nGUI_STARTUP_HOST_RESULT {rendered}", flush=True)
        return 0
    except Exception as exc:
        print(f"GUI_STARTUP_HOST_FAIL target={target} error={exc}", file=sys.stderr)
        return 1
    finally:
        sock.close()
        if console_log is not None:
            console_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
