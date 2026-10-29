#!/usr/bin/env python3
"""Run the Windows input window and its local QMP relay; omit --qmp for a probe."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def windows_path(path):
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmp", type=Path, help="QEMU Unix QMP socket; requires a relative mouse device")
    parser.add_argument("--self-test", action="store_true", help="compile and check Raw Input layouts without opening a window")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    repo = source.parent.parent
    if args.qmp and not args.qmp.is_socket():
        parser.error("--qmp must name a running QEMU Unix socket")
    artifacts = repo / ".artifacts" / "qemu-raw-mouse"
    artifacts.mkdir(parents=True, exist_ok=True)
    session = Path(tempfile.mkdtemp(prefix="session-", dir=artifacts))
    command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-File",
               windows_path(source / "windows_raw_mouse.ps1"),
               "-LogPath", windows_path(session / "input.log")]
    relay = None
    window = None
    with (session / "relay.log").open("w") as relay_log:
        try:
            if args.self_test:
                command.append("-SelfTest")
            elif args.qmp:
                ready = session / "proxy.json"
                relay = subprocess.Popen([sys.executable, str(source / "qmp_tcp_proxy.py"),
                                          str(args.qmp.resolve()), "--ready", str(ready)],
                                         stdout=relay_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 5
                while not ready.exists():
                    if relay.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("QMP relay failed to start; see " + str(session / "relay.log"))
                    time.sleep(.05)
                endpoint = json.loads(ready.read_text())
                command += ["-Endpoint", "%s:%s" % (endpoint["host"], endpoint["port"])]
            print("Input log:", session / "input.log", flush=True)
            window = subprocess.Popen(command)
            return window.wait()
        finally:
            if window is not None and window.poll() is None:
                window.terminate()
                window.wait()
            if relay is not None and relay.poll() is None:
                relay.terminate()
                relay.wait()


if __name__ == "__main__":
    raise SystemExit(main())
