#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import termios
import threading
import time
import tty
from pathlib import Path


OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_TEMPLATE = "/usr/share/OVMF/OVMF_VARS_4M.fd"


def qemu_cmd(run_dir: Path, disk: Path, nvme_disk: Path, usb_disk: Path | None, usb_hid: bool, qmp_port: int | None) -> list[str]:
    xhci_device = os.environ.get("KOBOX_SMOKE_XHCI_DEVICE", "qemu-xhci,id=xhci0")
    usb_mouse_device = os.environ.get("KOBOX_SMOKE_USB_MOUSE_DEVICE", "usb-mouse,bus=xhci0.0")
    cmd = [
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
        f"if=none,file={disk},format=qcow2,cache=writeback,aio=threads,id=bootdisk",
        "-device",
        "virtio-blk-pci,drive=bootdisk",
        "-drive",
        f"if=none,file={nvme_disk},format=raw,cache=writeback,aio=threads,id=nvmetest",
        "-device",
        "nvme,drive=nvmetest,serial=pachaos-nvme0",
    ]
    usb_iommu = os.environ.get("KOBOX_SMOKE_USB_IOMMU", "1") != "0"
    if (usb_disk is not None or usb_hid) and usb_iommu:
        cmd += [
            "-device",
            "intel-iommu,intremap=off",
        ]
    if usb_disk is not None or usb_hid:
        cmd += [
            "-device",
            xhci_device,
        ]
    if usb_disk is not None:
        cmd += [
            "-drive",
            f"if=none,file={usb_disk},format=raw,cache=writeback,aio=threads,id=usbstor0",
            "-device",
            "usb-storage,drive=usbstor0,bus=xhci0.0",
        ]
    if usb_hid:
        cmd += [
            "-device",
            usb_mouse_device,
        ]
        if qmp_port is not None:
            cmd += [
                "-qmp",
                f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
            ]
    cmd += [
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
        "virtio-net-pci,netdev=capnet0,mac=52:54:00:12:34:56",
    ]
    return cmd


def reserve_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def send_usb_mouse_qmp_events(port: int, delay_s: float, duration_s: float) -> None:
    deadline = time.monotonic() + 12.0
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        while True:
            try:
                sock.connect(("127.0.0.1", port))
                break
            except OSError:
                if time.monotonic() > deadline:
                    return
                time.sleep(0.05)

        def recv_some() -> None:
            sock.settimeout(1.0)
            try:
                sock.recv(4096)
            except OSError:
                pass

        def send_qmp(obj: object) -> bool:
            try:
                sock.sendall((json.dumps(obj) + "\r\n").encode("ascii"))
                recv_some()
                return True
            except OSError:
                return False

        recv_some()
        if not send_qmp({"execute": "qmp_capabilities"}):
            return
        time.sleep(delay_s)
        pattern = ((24, 6), (18, -5), (-12, 9), (7, -3), (10, 4), (-6, -6))
        stop = time.monotonic() + duration_s
        index = 0
        while time.monotonic() < stop:
            dx, dy = pattern[index % len(pattern)]
            if not send_qmp({
                "execute": "input-send-event",
                "arguments": {
                    "events": [
                        {"type": "rel", "data": {"axis": "x", "value": dx}},
                        {"type": "rel", "data": {"axis": "y", "value": dy}},
                    ],
                },
            }):
                return
            index += 1
            time.sleep(0.25)
    finally:
        sock.close()


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
        marker_id = marker.removeprefix("__KOBOX_SMOKE_DONE_").removesuffix("__")
        wrapped = f"({text}); __kobox_status=$?; printf '\\n__KOBOX_SMOKE_DONE_%s__:%d\\n' '{marker_id}' $__kobox_status"
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
    parser = argparse.ArgumentParser(description="Boot PachaOS and repeat kobox PachaOS device smokes.")
    parser.add_argument("--mode", choices=("nvme", "usb-storage", "usb-hid", "linux-abi"), default="nvme")
    parser.add_argument("--iterations", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--out", default=".artifacts/kobox-nvme-pachaos-smoke")
    parser.add_argument("--disk", default=".artifacts/disk.img")
    parser.add_argument("--command", default=None)
    args = parser.parse_args()
    if args.iterations <= 0:
        raise SystemExit("--iterations must be positive")

    root = Path.cwd()
    out_dir = root / args.out
    run_dir = Path("/tmp") / f"capabilityos-kobox-nvme-{os.getpid()}"
    shutil.rmtree(run_dir, ignore_errors=True)
    run_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    source_disk = Path(args.disk)
    if not source_disk.is_absolute():
        source_disk = root / source_disk
    boot_disk = run_dir / "disk-overlay.qcow2"
    nvme_disk = run_dir / "nvme-test.raw"
    usb_disk = run_dir / "usb-storage-test.raw" if args.mode == "usb-storage" else None
    usb_hid = args.mode == "usb-hid"
    qmp_port = reserve_tcp_port() if usb_hid else None
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw", "-b", str(source_disk), str(boot_disk)], check=True)
    subprocess.run(["qemu-img", "create", "-q", "-f", "raw", str(nvme_disk), "64M"], check=True)
    if usb_disk is not None:
        subprocess.run(["qemu-img", "create", "-q", "-f", "raw", str(usb_disk), "64M"], check=True)
    shutil.copyfile(OVMF_VARS_TEMPLATE, run_dir / "OVMF_VARS.fd")

    proc = subprocess.Popen(
        qemu_cmd(run_dir, boot_disk, nvme_disk, usb_disk, usb_hid, qmp_port),
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

        if args.command is not None:
            smoke_cmd = args.command
        elif args.mode == "usb-storage":
            smoke_cmd = (
                "i=1; "
                f"while [ \"$i\" -le {args.iterations} ]; do "
                "echo kobox-usb-storage-iter=$i; "
                "KOBOX_ENABLE_USB_EVENT_INJECT=1 KOBOX_USB_STORAGE_IO_SMOKE=1 KOBOX_USB_STORAGE_SUMMARY=1 "
                "/cmd/kobox-run.elf --backend=pachaos --drain-ms=250 "
                "--dep=/usr/lib/kobox/usbcore.ko "
                "--dep=/usr/lib/kobox/usb-storage.ko "
                "--dep=/usr/lib/kobox/xhci-hcd.ko "
                "run /usr/lib/kobox/xhci-pci.ko || exit $?; "
                "i=$((i + 1)); "
                "done"
            )
        elif args.mode == "usb-hid":
            smoke_cmd = (
                "KOBOX_USB_REAL_DEVICE=1 KOBOX_USB_PACHAOS_FAKE_ROOT_HUB=1 "
                "KOBOX_USB_HID_MOUSE_XHCI_ONLY=0 KOBOX_USB_SYNTHETIC_DEVICE=hid-mouse "
                "KOBOX_USB_HID_MOUSE_LIVE=1 KOBOX_USB_HID_MOUSE_LIVE_MS=1500 "
                "KOBOX_USB_HID_MOUSE_LIVE_PRINT_LIMIT=32 KOBOX_INPUT_SUMMARY=1 "
                "/cmd/kobox-run.elf --backend=pachaos --drain-ms=1500 "
                "--dep=/usr/lib/kobox/usbcore.ko "
                "--dep=/usr/lib/kobox/hid.ko "
                "--dep=/usr/lib/kobox/hid-generic.ko "
                "--dep=/usr/lib/kobox/usbhid.ko "
                "--dep=/usr/lib/kobox/xhci-hcd.ko "
                "run /usr/lib/kobox/xhci-pci.ko"
            )
        elif args.mode == "linux-abi":
            smoke_cmd = (
                "i=1; "
                f"while [ \"$i\" -le {args.iterations} ]; do "
                "echo linux-abi-probe-iter=$i; "
                "LINUX_ABI_PROBE_ROUNDS=12 LINUX_ABI_PROBE_THREADS=4 LINUX_ABI_PROBE_STRESS_WAVES=8 "
                "/cmd/linux_abi_probe.elf stress || exit $?; "
                "i=$((i + 1)); "
                "done"
            )
        else:
            smoke_cmd = (
                "i=1; "
                f"while [ \"$i\" -le {args.iterations} ]; do "
                "echo kobox-nvme-iter=$i; "
                "KOBOX_NVME_IO_SMOKE=1 /cmd/kobox-run.elf --backend=pachaos "
                "--dep=/usr/lib/kobox/nvme-auth.ko "
                "--dep=/usr/lib/kobox/nvme-core.ko "
                "run /usr/lib/kobox/nvme.ko || exit $?; "
                "i=$((i + 1)); "
                "done"
            )
        marker = f"__KOBOX_SMOKE_DONE_{os.getpid()}__"
        qmp_thread = None
        if usb_hid and qmp_port is not None:
            qmp_thread = threading.Thread(
                target=send_usb_mouse_qmp_events,
                args=(qmp_port, 1.5, min(max(args.timeout - 3.0, 1.0), 8.0)),
                daemon=True,
            )
            qmp_thread.start()
        start = time.monotonic()
        out, status = console.command(smoke_cmd, args.timeout, marker)
        if qmp_thread is not None:
            qmp_thread.join(timeout=2.0)
        elapsed = time.monotonic() - start
        text = out.decode("utf-8", errors="replace")
        print("--- timing ---")
        print(f"boot_wait_s={boot_wait_s:.3f}")
        print(f"command_s={elapsed:.3f}")
        print("--- command output ---")
        print(text)

        if args.command is not None:
            return status

        if args.mode == "usb-storage":
            ok_count = text.count("kobox-usb-storage-bot:")
        elif args.mode == "usb-hid":
            ok_count = text.count("kobox-usb-hid-mouse-live-summary:") if "result=ok" in text else 0
        elif args.mode == "linux-abi":
            ok_count = text.count("linux_abi_probe: stress ok")
        else:
            ok_count = text.count("kobox nvme io smoke: cases=3 reset=ok")
        expected = 1 if args.mode == "usb-hid" else args.iterations
        if status != 0 or ok_count != expected or "PAGE FAULT" in text or "Segmentation fault" in text:
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
            src = run_dir / name
            if src.exists():
                shutil.copyfile(src, out_dir / name)
        if console is not None:
            (out_dir / "console.log").write_bytes(console.log)
        if qemu_stdout:
            (out_dir / "qemu-stdout.log").write_text("".join(qemu_stdout), encoding="utf-8")
        shutil.rmtree(run_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
