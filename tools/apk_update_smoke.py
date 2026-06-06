#!/usr/bin/env python3
import argparse
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


def qemu_cmd(run_dir: Path, disk: Path, disk_format: str) -> list[str]:
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
        f"if=none,file={disk},format={disk_format},cache=writeback,aio=threads,id=bootdisk",
        "-device",
        "virtio-blk-pci,drive=bootdisk",
        "-device",
        "qemu-xhci,id=xhci0,p2=1,p3=1",
        "-device",
        "usb-mouse,bus=xhci0.0",
        "-serial",
        f"file:{run_dir / 'serial.log'}",
        "-device",
        "virtio-serial-pci",
        "-chardev",
        "pty,id=capconsole",
        "-device",
        "virtconsole,chardev=capconsole,name=capabilityos.console.0",
        "-netdev",
        "user,id=capnet0,ipv6=off,dhcpstart=10.0.2.15",
        "-device",
        "virtio-net-pci,netdev=capnet0,mac=52:54:00:12:34:6a",
    ]


class Console:
    def __init__(self, fd: int):
        self.fd = fd
        self.log = bytearray()

    def read_until(self, needle: bytes, timeout: float) -> bytes:
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
                if needle in buf:
                    return bytes(buf)
            time.sleep(0.01)
        raise TimeoutError(f"timeout waiting for {needle!r}; tail={bytes(buf[-2000:])!r}")

    def command(self, text: str, timeout: float, marker: str) -> tuple[bytes, int]:
        marker_id = marker.removeprefix("__CAPABILITYOS_SMOKE_DONE_").removesuffix("__")
        wrapped = f"{text}; printf '\\n__CAPABILITYOS_SMOKE_DONE_%s__:%d\\n' '{marker_id}' $?"
        os.write(self.fd, wrapped.encode("ascii") + b"\r")
        out = self.read_until(marker.encode("ascii") + b":", timeout)
        status = 1
        marker_bytes = marker.encode("ascii") + b":"
        marker_pos = out.rfind(marker_bytes)
        if marker_pos >= 0:
            status_start = marker_pos + len(marker_bytes)
            status_end = out.find(b"\n", status_start)
            if status_end < 0:
                out += self.read_until(b"\n", 1.0)
                status_end = out.find(b"\n", status_start)
            if status_end < 0:
                status_end = len(out)
            try:
                status = int(out[status_start:status_end].strip() or b"1")
            except ValueError:
                status = 1
        return out, status


def main() -> int:
    parser = argparse.ArgumentParser(description="Boot CapabilityOS and run one apk-related command.")
    parser.add_argument("--out", default=".artifacts/apk-update-smoke")
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--command", default="apk update")
    parser.add_argument("--disk", default=None, help="disk image to boot; defaults to .artifacts/disk.img")
    parser.add_argument("--disk-format", choices=("raw", "qcow2"), default=None, help="QEMU disk format for --in-place; defaults to raw for .img and qcow2 for .qcow2")
    parser.add_argument("--in-place", action="store_true", help="boot the selected disk directly instead of a temporary copy")
    args = parser.parse_args()

    root = Path.cwd()
    out_dir = root / args.out
    runtime_dir = Path("/tmp") / f"capabilityos-apk-update-smoke-{os.getpid()}"
    shutil.rmtree(runtime_dir, ignore_errors=True)
    runtime_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    source_disk = Path(args.disk) if args.disk is not None else root / ".artifacts" / "disk.img"
    if not source_disk.is_absolute():
        source_disk = root / source_disk
    disk_format = args.disk_format if args.disk_format is not None else ("qcow2" if source_disk.suffix == ".qcow2" else "raw")
    if args.in_place:
        disk = source_disk
    else:
        disk = runtime_dir / "disk-overlay.qcow2"
        subprocess.run(
            [
                "qemu-img",
                "create",
                "-q",
                "-f",
                "qcow2",
                "-F",
                "raw",
                "-b",
                str(source_disk),
                str(disk),
            ],
            check=True,
        )
        disk_format = "qcow2"
    shutil.copyfile(OVMF_VARS_TEMPLATE, runtime_dir / "OVMF_VARS.fd")

    proc = subprocess.Popen(
        qemu_cmd(runtime_dir, disk, disk_format),
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
            raise TimeoutError("console pty not announced")

        fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        old_attrs = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        console = Console(fd)
        boot_start = time.monotonic()
        console.read_until(b"# ", 45.0)
        boot_wait_s = time.monotonic() - boot_start
        command_start = time.monotonic()
        marker = f"__CAPABILITYOS_SMOKE_DONE_{os.getpid()}__"
        out, command_status = console.command(args.command, args.timeout, marker)
        command_s = time.monotonic() - command_start
        print("--- timing ---")
        print(f"boot_wait_s={boot_wait_s:.3f}")
        print(f"command_s={command_s:.3f}")
        print("--- command output ---")
        print(out.decode("utf-8", errors="replace"))
        if (
            b"ERROR:" in out
            or b"I/O error" in out
            or b"not found" in out
            or b"BAD signature" in out
            or b"temporary error" in out
            or b"unavailable" in out
            or b"Permission denied" in out
            or command_status != 0
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
        for name in ("serial.log", "qemu.log"):
            src = runtime_dir / name
            if src.exists():
                shutil.copyfile(src, out_dir / name)
        if console is not None:
            (out_dir / "console.log").write_bytes(console.log)
        if qemu_stdout:
            (out_dir / "qemu-stdout.log").write_text("".join(qemu_stdout), encoding="utf-8")
        shutil.rmtree(runtime_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
