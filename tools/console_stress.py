#!/usr/bin/env python3
import argparse
import concurrent.futures
import os
import re
import shutil
import signal
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path


OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_TEMPLATE = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


class PtyReader:
    def __init__(self, fd: int):
        self.fd = fd
        self.buffer = bytearray()

    def read_until(self, needle: bytes, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = self.buffer.find(needle)
            if found >= 0:
                end = found + len(needle)
                out = bytes(self.buffer[:end])
                del self.buffer[:end]
                return out
            try:
                chunk = os.read(self.fd, 8192)
            except BlockingIOError:
                chunk = b""
            if chunk:
                self.buffer.extend(chunk)
            time.sleep(0.01)
        tail = bytes(self.buffer[-2000:])
        raise TimeoutError(f"timeout waiting for {needle!r}; tail={tail!r}")


def write_all(fd: int, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        offset += os.write(fd, data[offset:])


def command(reader: PtyReader, fd: int, text: str, timeout: float) -> bytes:
    write_all(fd, text.encode("ascii") + b"\r")
    out = reader.read_until(b"# ", timeout)
    plain = ANSI_RE.sub(b"", out)
    if b"/cmd/dash_interactive.elf:" in plain:
        raise RuntimeError(f"dash reported command error for {text!r}: {plain!r}")
    if b"not found" in plain:
        raise RuntimeError(f"lookup failed for {text!r}: {plain!r}")
    if b"Syntax error" in plain:
        raise RuntimeError(f"syntax error for {text!r}: {plain!r}")
    return plain


def expect_contains(label: str, out: bytes, required: list[bytes]) -> None:
    missing = [item for item in required if item not in out]
    if missing:
        raise RuntimeError(f"{label} missing {missing!r}: {out!r}")


def qemu_cmd(run_dir: Path, disk: Path, run_id: int) -> list[str]:
    mac_tail = 0x56 + (run_id & 0x3F)
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
        "-d",
        "guest_errors,cpu_reset",
        "-D",
        str(run_dir / "qemu.log"),
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive",
        f"if=pflash,format=raw,file={run_dir / 'OVMF_VARS.fd'}",
        "-drive",
        f"if=none,file={disk},format=raw,id=bootdisk",
        "-device",
        "virtio-blk-pci,drive=bootdisk",
        "-netdev",
        "user,id=capnet0,ipv6=off,dhcpstart=10.0.2.15",
        "-device",
        f"virtio-net-pci,netdev=capnet0,mac=52:54:00:12:34:{mac_tail:02x}",
        "-serial",
        f"file:{run_dir / 'serial.log'}",
        "-device",
        "virtio-serial-pci",
        "-chardev",
        "pty,id=capconsole",
        "-device",
        "virtconsole,chardev=capconsole,name=capabilityos.console.0",
    ]


def run_one(root: Path, out_dir: Path, run_id: int, loops: int, prompt_timeout: float) -> tuple[int, str]:
    run_dir = out_dir / f"run-{run_id}"
    runtime_dir = Path("/tmp") / f"capabilityos-console-stress-{os.getpid()}-{run_id}"
    shutil.rmtree(runtime_dir, ignore_errors=True)
    runtime_dir.mkdir(parents=True, exist_ok=True)
    run_dir.mkdir(parents=True, exist_ok=True)
    for old in ("serial.log", "qemu.log", "console.log"):
        try:
            (run_dir / old).unlink()
        except FileNotFoundError:
            pass

    disk = runtime_dir / "disk.img"
    shutil.copyfile(root / ".artifacts" / "disk.img", disk)
    shutil.copyfile(OVMF_VARS_TEMPLATE, runtime_dir / "OVMF_VARS.fd")
    proc = subprocess.Popen(
        qemu_cmd(runtime_dir, disk, run_id),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    qemu_stdout: list[str] = []
    console_log = bytearray()
    fd = None
    old_attrs = None
    command_count = 0
    try:
        pty_path = None
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            line = proc.stdout.readline() if proc.stdout else ""
            if line:
                qemu_stdout.append(line.rstrip())
                qemu_stdout = qemu_stdout[-20:]
            if not line and proc.poll() is not None:
                raise RuntimeError(f"qemu exited early: {proc.returncode}; stdout={qemu_stdout!r}")
            match = re.search(r"char device redirected to (\S+)", line)
            if match:
                pty_path = match.group(1)
                break
        if pty_path is None:
            raise TimeoutError("console pty not announced")

        fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        old_attrs = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        reader = PtyReader(fd)
        boot = reader.read_until(b"# ", 45.0)
        console_log.extend(boot)

        for loop in range(loops):
            checks: list[tuple[str, float, list[bytes]]] = [
                ("pwd", 10.0, [b"/"]),
                ("ls", 15.0, [b"bin", b"cmd", b"dev"]),
                ("fastfetch", 30.0, [b"PachaOS", b"Shell:", b"dash", b"Terminal:"]),
                ("apk --version", 20.0, [b"apk-tools"]),
                ("zstd --version", 15.0, [b"Zstandard CLI"]),
                ("cat /etc/os-release", 10.0, [b'NAME="PachaOS"']),
                ("true", 10.0, []),
                ("echo simple-ok", 10.0, [b"simple-ok"]),
                ("cd bin", 10.0, []),
                ("pwd", 10.0, [b"/bin"]),
                ("ls", 15.0, [b"coreutils", b"zstd", b"fastfetch"]),
                ("cd /", 10.0, []),
            ]
            for text, timeout, required in checks:
                out = command(reader, fd, text, max(timeout, prompt_timeout))
                console_log.extend(out)
                command_count += 1
                if required:
                    expect_contains(f"run {run_id} loop {loop} {text}", out, required)

        serial_text = ""
        serial_path = runtime_dir / "serial.log"
        if serial_path.exists():
            serial_text = serial_path.read_text(encoding="utf-8", errors="replace")
        bad_markers = [
            "ExecLoader: child faulted",
            "PAGE FAULT",
            "#PF",
            "GENERAL PROTECTION",
            "INVALID OPCODE",
            "STACK SEGMENT FAULT",
            "SEGMENT NOT PRESENT",
        ]
        for marker in bad_markers:
            if marker in serial_text:
                raise RuntimeError(f"serial contains {marker!r}")

        return command_count, f"run-{run_id}: ok ({command_count} commands)"
    except Exception as exc:
        return command_count, f"run-{run_id}: failed after {command_count} commands: {exc}"
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
        for name in ("serial.log", "qemu.log"):
            src = runtime_dir / name
            if src.exists():
                shutil.copyfile(src, run_dir / name)
        (run_dir / "console.log").write_bytes(console_log)
        if qemu_stdout:
            (run_dir / "qemu-stdout.log").write_text("\n".join(qemu_stdout) + "\n", encoding="utf-8")
        shutil.rmtree(runtime_dir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run parallel CapabilityOS virtio-console command stress.")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--loops", type=int, default=6)
    parser.add_argument("--prompt-timeout", type=float, default=10.0)
    parser.add_argument("--out", default=".artifacts/console-stress")
    args = parser.parse_args()

    if args.jobs < 1 or args.jobs > 16:
        parser.error("--jobs must be between 1 and 16")
    if args.loops < 1:
        parser.error("--loops must be positive")

    root = Path.cwd()
    out_dir = root / args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    summary_path = out_dir / "summary.txt"

    status = 0
    total = 0
    lines: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [
            executor.submit(run_one, root, out_dir, run_id, args.loops, args.prompt_timeout)
            for run_id in range(1, args.jobs + 1)
        ]
        for future in concurrent.futures.as_completed(futures):
            count, line = future.result()
            total += count
            lines.append(line)
            print(line, flush=True)
            if "failed" in line:
                status = 1

    lines.sort()
    lines.append(f"total commands: {total}")
    lines.append(f"result: {'ok' if status == 0 else 'failed'}")
    summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"logs: {out_dir}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
