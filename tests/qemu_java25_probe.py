#!/usr/bin/env python3
"""Drive the Java 25 probe through an existing guest console (no rootfs sync).

Use with pacgo qemu-test --python. A console-shell profile must already be
selected. JAVA25_INSTALL=1 transfers only the pinned JDK and probe over HTTP
to the running guest, avoiding writes beneath an ext4 journal from debugfs.
"""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import select
import socket
import sys
import tarfile
import threading
import time


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


def main():
    repo = Path(__file__).resolve().parents[1]
    out = repo / ".artifacts/java25"
    (out / "guest-results.json").unlink(missing_ok=True)
    serve = out / "serve"
    serve.mkdir(parents=True, exist_ok=True)
    archive = repo / ".artifacts/third_party/java25/zulu25.38.107-ca-jdk25.0.5-linux_musl_x64.tar.gz"
    link = serve / "jdk.tar.gz"
    if not link.exists():
        link.symlink_to(archive)
    lpr = serve / "lpr.so"
    if not lpr.exists():
        lpr.symlink_to(repo / ".artifacts/lpr-linux-x86_64.so")
    with tarfile.open(serve / "probe.tar.gz", "w:gz") as tar:
        for path in sorted((out / "classes").glob("*.class")):
            tar.add(path, arcname="opt/java25-tests/" + path.name)
        tar.add(repo / "tests/fixtures/java25/probe.sh", arcname="cmd/java25_probe.sh")
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(QuietHandler, directory=serve))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = "http://10.0.2.2:" + str(server.server_port)
    console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    console.connect(os.environ["PACGO_QEMU_CONSOLE"])
    console.setblocking(False)
    log = open(os.environ["PACGO_QEMU_CONSOLE_LOG"], "ab", buffering=0)
    command_id = 0

    def receive_until(pattern, seconds):
        data = bytearray()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if not select.select([console], [], [], .1)[0]:
                continue
            chunk = console.recv(65536)
            if not chunk:
                raise EOFError("guest console closed")
            log.write(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            data.extend(chunk)
            match = re.search(pattern, data)
            if match:
                return bytes(data), match
        raise TimeoutError("guest command timed out: " + repr(pattern))

    def run(command, seconds=60, required=True):
        nonlocal command_id
        command_id += 1
        # Construct the completion marker in printf so a TTY echo of the
        # command cannot be mistaken for successful execution.
        tail = "; __java_status=$?; printf '\\nJAVA25_COMMAND_%s_STATUS=%s\\n' "
        tail += str(command_id) + ' "$__java_status"'
        console.sendall((command + tail + "\n").encode())
        pattern = rb"[\r\n]JAVA25_COMMAND_" + str(command_id).encode() + rb"_STATUS=([0-9]+)[\r\n]"
        data, match = receive_until(pattern, seconds)
        status = int(match[1])
        if required and status:
            raise RuntimeError("guest command exited " + str(status) + ": " + command)
        return data, status

    try:
        console.sendall(b"\n")
        receive_until(rb"bash-[0-9.]+# ", 45)
        if os.environ.get("JAVA25_UPDATE_LPR") == "1":
            run("/bin/busybox wget -q -O /tmp/java25-lpr.so " + base + "/lpr.so && chmod 755 /tmp/java25-lpr.so && mv /tmp/java25-lpr.so /lib/pacha/lpr-linux-x86_64.so")
        if os.environ.get("JAVA25_INSTALL") == "1":
            run("/bin/busybox wget -q -O /tmp/java25.tar.gz " + base + "/jdk.tar.gz", 120)
            run("mkdir -p /opt/java25 && /bin/busybox tar -xzf /tmp/java25.tar.gz --strip-components=1 -C /opt/java25 && rm /tmp/java25.tar.gz", 180)
        run("/bin/busybox wget -q -O /tmp/java25-probe.tar.gz " + base + "/probe.tar.gz && /bin/busybox tar -xzf /tmp/java25-probe.tar.gz -C / && rm /tmp/java25-probe.tar.gz", 60)
        run("/opt/java25/bin/java -Xms32m -Xmx128m -XX:-CreateCoredumpOnCrash '-XX:ErrorFile=/tmp/java25-hs_err_pid%p.log' -version", 90)
        failures = []
        results = []
        cases = os.environ.get("JAVA25_CASES", "null-field null-array-length null-array-element null-invoke threads stack gc").split()
        modes = os.environ.get("JAVA25_MODES", "int jit").split()
        for mode in modes:
            for case in cases:
                data, status = run("/bin/sh /cmd/java25_probe.sh " + mode + " " + case, 90, False)
                passed = status == 0 and b"JAVA25_PROBE=PASS case=" + case.encode() in data
                compiled = None
                if mode == "jit" and case.startswith("null"):
                    compiled = bool(re.search(rb"\bb\s+4\s+Java25Probe::(?:arrayLength|arrayElement|invoke)\b", data))
                    passed = passed and compiled
                results.append(dict(mode=mode, case=case, exit_status=status,
                                    c2_compiled=compiled, passed=passed))
                if not passed:
                    failures.append(mode + "/" + case)
        (out / "guest-results.json").write_text(json.dumps(results, indent=2) + "\n")
        run("sync")
        if failures:
            run("for f in /tmp/java25-hs_err_pid*.log; do test ! -f \"$f\" || /bin/busybox head -n 100 \"$f\"; done", required=False)
            raise RuntimeError("Java 25 probe failed: " + ", ".join(failures))
        print("JAVA25_GUEST=PASS", flush=True)
    finally:
        # Even a failed extraction or JVM launch must flush completed guest
        # writes before pacgo stops QEMU; otherwise the next boot loses them.
        try:
            if os.environ.get("JAVA25_RESTORE_STARTX") == "1":
                run(": > /etc/pacha_boot_profile && sync", 20)
            else:
                run("sync", 20, False)
        except (OSError, EOFError, TimeoutError):
            pass
        server.shutdown()
        console.close()
        log.close()


if __name__ == "__main__":
    main()
