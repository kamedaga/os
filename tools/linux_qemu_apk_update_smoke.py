#!/usr/bin/env python3
import argparse
import os
import re
import signal
import shutil
import subprocess
import termios
import time
import tty
from pathlib import Path


def qemu_cmd(run_dir: Path, iso: Path) -> list[str]:
    return [
        "qemu-system-x86_64",
        "-enable-kvm",
        "-cpu",
        "host",
        "-machine",
        "q35",
        "-smp",
        "4",
        "-m",
        "2G",
        "-no-reboot",
        "-monitor",
        "none",
        "-display",
        "none",
        "-serial",
        "pty",
        "-drive",
        f"if=none,media=cdrom,file={iso},id=cdrom",
        "-device",
        "virtio-blk-pci,drive=cdrom",
        "-netdev",
        "user,id=capnet0,ipv6=off,dhcpstart=10.0.2.15",
        "-device",
        "virtio-net-pci,netdev=capnet0,mac=52:54:00:12:34:6a",
    ]


class Console:
    def __init__(self, fd: int):
        self.fd = fd
        self.log = bytearray()

    def read_until_any(self, needles: list[bytes], timeout: float) -> bytes:
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = os.read(self.fd, 8192)
            except BlockingIOError:
                chunk = b""
            if chunk:
                buf.extend(chunk)
                self.log.extend(chunk)
                if any(needle in buf for needle in needles):
                    return bytes(buf)
            time.sleep(0.01)
        raise TimeoutError(f"timeout waiting for {needles!r}; tail={bytes(buf[-2000:])!r}")

    def command(self, text: str, timeout: float) -> bytes:
        os.write(self.fd, text.encode("ascii") + b"\r")
        return self.read_until_any([b"\nlocalhost:~# ", b"\r\nlocalhost:~# "], timeout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Boot Alpine Linux in QEMU and run apk update.")
    parser.add_argument("--iso", default=".artifacts/alpine-qemu/alpine-virt-3.22.4-x86_64.iso")
    parser.add_argument("--out", default=".artifacts/linux-qemu-apk-update")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--pre-command", default="")
    parser.add_argument("--command", default="rm -f /var/cache/apk/APKINDEX.*; apk update")
    args = parser.parse_args()

    root = Path.cwd()
    iso = root / args.iso
    out_dir = root / args.out
    runtime_dir = Path("/tmp") / f"linux-qemu-apk-update-{os.getpid()}"
    shutil.rmtree(runtime_dir, ignore_errors=True)
    runtime_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    proc = subprocess.Popen(
        qemu_cmd(runtime_dir, iso),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    fd = None
    old_attrs = None
    console = None
    qemu_stdout: list[str] = []
    try:
        pty_path = None
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            line = proc.stdout.readline() if proc.stdout else ""
            if line:
                print(line.rstrip(), flush=True)
                qemu_stdout.append(line)
            if not line and proc.poll() is not None:
                raise RuntimeError(f"qemu exited early: {proc.returncode}")
            match = re.search(r"char device redirected to (\S+)", line)
            if match:
                pty_path = match.group(1)
                break
        if pty_path is None:
            raise TimeoutError("serial pty not announced")

        fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        old_attrs = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        console = Console(fd)

        boot_start = time.monotonic()
        first = console.read_until_any([b"boot:", b"localhost login:"], 45.0)
        if b"boot:" in first:
            os.write(fd, b"\r")
            console.read_until_any([b"localhost login:"], 45.0)
        os.write(fd, b"root\r")
        console.read_until_any([b"\nlocalhost:~# ", b"\r\nlocalhost:~# "], 20.0)
        boot_wait_s = time.monotonic() - boot_start

        setup_cmd = (
            "ip link set eth0 up; "
            "udhcpc -i eth0 -q; "
            "printf '%s\\n' "
            "'http://dl-cdn.alpinelinux.org/alpine/v3.22/main' "
            "'http://dl-cdn.alpinelinux.org/alpine/v3.22/community' "
            "> /etc/apk/repositories"
        )
        setup_out = console.command(setup_cmd, 30.0)
        print("--- setup output ---")
        print(setup_out.decode("utf-8", errors="replace"))
        if args.pre_command:
            pre_out = console.command(args.pre_command, args.timeout)
            print("--- pre-command output ---")
            print(pre_out.decode("utf-8", errors="replace"))

        for run in range(args.runs):
            command_start = time.monotonic()
            out = console.command(args.command, args.timeout)
            command_s = time.monotonic() - command_start
            print(f"--- run {run + 1} timing ---")
            print(f"boot_wait_s={boot_wait_s:.3f}")
            print(f"command_s={command_s:.3f}")
            print("--- command output ---")
            text = out.decode("utf-8", errors="replace")
            print(text)
            if (
                "ERROR:" in text
                or "BAD signature" in text
                or "temporary error" in text
                or "unavailable" in text
            ):
                return 1
        return 0
    finally:
        if fd is not None:
            if old_attrs is not None:
                try:
                    termios.tcsetattr(fd, termios.TCSANOW, old_attrs)
                except termios.error:
                    pass
            os.close(fd)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        if proc.stdout is not None:
            try:
                rest = proc.stdout.read()
                if rest:
                    qemu_stdout.append(rest)
            except Exception:
                pass
        (out_dir / "qemu-stdout.log").write_text("".join(qemu_stdout), encoding="utf-8")
        if console is not None:
            (out_dir / "console.log").write_bytes(console.log)


if __name__ == "__main__":
    raise SystemExit(main())
