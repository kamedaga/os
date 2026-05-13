#!/usr/bin/env python3
import argparse
import os
import random
import re
import shutil
import signal
import subprocess
import termios
import time
import tty
from pathlib import Path


OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_TEMPLATE = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
BAD_SERIAL_MARKERS = (
    "PAGE FAULT",
    "#PF",
    "GENERAL PROTECTION",
    "INVALID OPCODE",
    "STACK SEGMENT FAULT",
    "SEGMENT NOT PRESENT",
    "ExecLoader: child faulted",
)


class Console:
    def __init__(self, fd: int):
        self.fd = fd
        self.buffer = bytearray()
        self.log = bytearray()

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
                self.log.extend(chunk)
            time.sleep(0.01)
        tail = bytes(self.buffer[-3000:])
        raise TimeoutError(f"timeout waiting for {needle!r}; tail={tail!r}")

    def write_line(self, text: str) -> None:
        data = text.encode("ascii") + b"\r\n"
        offset = 0
        while offset < len(data):
            offset += os.write(self.fd, data[offset:])


def qemu_cmd(run_dir: Path, disk: Path, smp: int) -> list[str]:
    return [
        "qemu-system-x86_64",
        "-enable-kvm",
        "-cpu",
        "host",
        "-machine",
        "q35",
        "-smp",
        str(smp),
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
        "virtio-net-pci,netdev=capnet0,mac=52:54:00:12:34:77",
        "-serial",
        f"file:{run_dir / 'serial.log'}",
        "-device",
        "virtio-serial-pci",
        "-chardev",
        "pty,id=capconsole",
        "-device",
        "virtconsole,chardev=capconsole,name=capabilityos.console.0",
    ]


def base_commands() -> list[tuple[str, str, tuple[bytes, ...]]]:
    return [
        ("printf-cat", r"printf 'a\nb\nc\n' | cat", (b"a", b"b", b"c")),
        ("printf-wc", r"printf 'a\nb\nc\n' | wc -l", (b"3",)),
        ("printf-tr", r"printf 'hello\n' | tr a-z A-Z", (b"HELLO",)),
        ("seq-cat", "seq 1 5 | cat", (b"1", b"5")),
        ("seq-head3", "seq 1 10 | head -n 3", (b"1", b"2", b"3")),
        ("seq-head1", "seq 1 1000 | head -n 1", (b"1",)),
        (
            "seq-head1-profile",
            "CAPABILITYOS_EXEC_PROFILE=1 CAPABILITYOS_EXEC_PROFILE_VERBOSE=1 seq 1 1000 | CAPABILITYOS_EXEC_PROFILE=1 CAPABILITYOS_EXEC_PROFILE_VERBOSE=1 head -n 1",
            (b"1",),
        ),
        (
            "seq-head1-profile-quiet",
            "CAPABILITYOS_EXEC_PROFILE=1 seq 1 1000 | CAPABILITYOS_EXEC_PROFILE=1 head -n 1",
            (b"1",),
        ),
        ("seq-head-wc", "seq 1 20 | head -n 11 | wc -l", (b"11",)),
        ("printf-cat-wc", r"printf 'z\nb\na\n' | cat | wc -l", (b"3",)),
        ("seq-head-wc3", "seq 1 10 | head -n 3 | wc -l", (b"3",)),
        ("ls-wc", "ls / | wc -l", ()),
        ("cat-head-wc", "cat /bin/ls | head -c 16 | wc -c", (b"16",)),
        ("seq-head3-repeat", "seq 1 3 | head -n 3", (b"1", b"2", b"3")),
        ("printf-readv", r"printf '0123456789abcdef\n' | wc -c", (b"17",)),
    ]


def make_command(index: int, body: str) -> tuple[bytes, bytes, str]:
    begin = f"__PIPE_BEGIN_{index:04d}__"
    end = f"__PIPE_END_{index:04d}__"
    wrapped = f"printf '{begin}\\n'; {body}; rc=$?; printf '\\n{end}:%s\\n' \"$rc\""
    return begin.encode("ascii"), end.encode("ascii"), wrapped


def run_pipe_command(console: Console, index: int, label: str, body: str, required: tuple[bytes, ...], timeout: float, send_delay: float) -> tuple[bytes, dict[str, float]]:
    begin, end, wrapped = make_command(index, body)
    if send_delay > 0:
        time.sleep(send_delay)
    sent_at = time.monotonic()
    console.write_line(wrapped)
    wrote_at = time.monotonic()
    console.read_until(b"\r\n", timeout)
    echo_at = time.monotonic()
    console.read_until(begin, timeout)
    begin_at = time.monotonic()
    out = console.read_until(end, timeout)
    end_at = time.monotonic()
    tail = console.read_until(b"# ", timeout)
    prompt_at = time.monotonic()
    plain = ANSI_RE.sub(b"", out + tail)
    if b"/cmd/dash_interactive.elf:" in plain or b"I/O error" in plain or b"not found" in plain:
        raise RuntimeError(f"{label} reported shell error: {plain!r}")
    body_start = plain.rfind(begin)
    body_end = plain.rfind(end)
    payload = plain[body_start + len(begin) : body_end] if body_start >= 0 and body_end > body_start else plain
    missing = [item for item in required if item not in payload]
    if missing:
        raise RuntimeError(f"{label} missing {missing!r}: {plain!r}")
    if label in ("seq-head1", "seq-head1-profile", "seq-head1-profile-quiet") and b"\n2" in payload.replace(b"\r\n", b"\n"):
        raise RuntimeError(f"{label} produced more than one line: {plain!r}")
    if b":0" not in plain[: plain.find(b"# ") if b"# " in plain else len(plain)]:
        raise RuntimeError(f"{label} non-zero or missing rc: {plain!r}")
    timing = {
        "write_s": wrote_at - sent_at,
        "echo_s": echo_at - wrote_at,
        "begin_s": begin_at - echo_at,
        "command_s": end_at - begin_at,
        "prompt_s": prompt_at - end_at,
        "total_s": prompt_at - sent_at,
    }
    return plain, timing


def copy_logs(runtime_dir: Path, out_dir: Path, console: Console | None, qemu_stdout: list[str], failure: str | None) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for name in ("serial.log", "qemu.log"):
        src = runtime_dir / name
        if src.exists():
            shutil.copyfile(src, out_dir / name)
    if console is not None:
        (out_dir / "console.log").write_bytes(console.log)
    if qemu_stdout:
        (out_dir / "qemu-stdout.log").write_text("".join(qemu_stdout), encoding="utf-8")
    if failure is not None:
        (out_dir / "failure.txt").write_text(failure + "\n", encoding="utf-8")


def write_results(out_dir: Path, completed: list[str], timings: list[tuple[int, str, dict[str, float]]], result: str) -> None:
    timing_lines = ["index\tlabel\twrite_s\techo_s\tbegin_s\tcommand_s\tprompt_s\ttotal_s"]
    for item_index, item_label, timing in timings:
        timing_lines.append(
            f"{item_index}\t{item_label}\t{timing['write_s']:.6f}\t{timing['echo_s']:.6f}\t{timing['begin_s']:.6f}\t{timing['command_s']:.6f}\t{timing['prompt_s']:.6f}\t{timing['total_s']:.6f}"
        )
    (out_dir / "timings.tsv").write_text("\n".join(timing_lines) + "\n", encoding="utf-8")
    (out_dir / "summary.txt").write_text("\n".join(completed + [result]) + "\n", encoding="utf-8")


def check_serial(runtime_dir: Path) -> None:
    serial_path = runtime_dir / "serial.log"
    if not serial_path.exists():
        return
    serial = serial_path.read_text(encoding="utf-8", errors="replace")
    for marker in BAD_SERIAL_MARKERS:
        if marker in serial:
            raise RuntimeError(f"serial contains {marker!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Stress Linux ABI pipe/exec/wait behavior through the virtio console.")
    parser.add_argument("--loops", type=int, default=40)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=12.0)
    parser.add_argument("--out", default=".artifacts/pipe-stress")
    parser.add_argument("--only", help="run only the command with this label")
    parser.add_argument("--exclude", action="append", default=[], help="exclude a command label; may be repeated or comma-separated")
    parser.add_argument("--body", help="run this exact shell body for every loop")
    parser.add_argument("--label", default="custom", help="label to use with --body")
    parser.add_argument("--expect", action="append", default=[], help="required output substring for --body")
    parser.add_argument("--smp", type=int, default=4)
    parser.add_argument("--send-delay", type=float, default=0.05)
    parser.add_argument("--keep-runtime", action="store_true")
    args = parser.parse_args()

    if os.name == "nt":
        raise SystemExit("run this under WSL/Linux: wsl -e bash -lc 'cd /mnt/c/.../CapabilityOS && python3 tools/pipe_stress.py'")
    if args.loops < 1:
        parser.error("--loops must be positive")

    random.seed(args.seed)
    root = Path.cwd()
    out_dir = root / args.out
    runtime_dir = Path("/tmp") / f"capabilityos-pipe-stress-{os.getpid()}"
    shutil.rmtree(runtime_dir, ignore_errors=True)
    runtime_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    disk = runtime_dir / "disk.img"
    shutil.copyfile(root / ".artifacts" / "disk.img", disk)
    shutil.copyfile(OVMF_VARS_TEMPLATE, runtime_dir / "OVMF_VARS.fd")

    proc = subprocess.Popen(
        qemu_cmd(runtime_dir, disk, args.smp),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    fd = None
    old_attrs = None
    console = None
    qemu_stdout: list[str] = []
    completed: list[str] = []
    timings: list[tuple[int, str, dict[str, float]]] = []
    failure: str | None = None
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
        console.read_until(b"# ", 45.0)

        if args.body:
            commands = [(args.label, args.body, tuple(item.encode("utf-8") for item in args.expect))]
        else:
            commands = base_commands()
        if args.only:
            commands = [cmd for cmd in commands if cmd[0] == args.only]
            if not commands:
                raise RuntimeError(f"unknown command label: {args.only}")
        excludes = {item for group in args.exclude for item in group.split(",") if item}
        if excludes:
            commands = [cmd for cmd in commands if cmd[0] not in excludes]
            if not commands:
                raise RuntimeError("all command labels were excluded")
        for index in range(1, args.loops + 1):
            label, body, required = commands[(index - 1) % len(commands)]
            if index > len(commands) and not args.only:
                label, body, required = random.choice(commands)
            print(f"{index:04d} {label}: {body}", flush=True)
            out, timing = run_pipe_command(console, index, label, body, required, args.timeout, args.send_delay)
            timings.append((index, label, timing))
            completed.append(f"{index:04d} {label} ok total={timing['total_s']:.3f}s command={timing['command_s']:.3f}s prompt={timing['prompt_s']:.3f}s")
            print(completed[-1], flush=True)
            (out_dir / "last-output.txt").write_bytes(out)
            check_serial(runtime_dir)

        summary = "\n".join(completed + [f"result: ok ({len(completed)} commands)"]) + "\n"
        write_results(out_dir, completed, timings, f"result: ok ({len(completed)} commands)")
        print(summary, end="")
        return 0
    except Exception as exc:
        failure = f"failed after {len(completed)} commands: {exc}"
        print(failure, flush=True)
        write_results(out_dir, completed, timings, failure)
        return 1
    finally:
        copy_logs(runtime_dir, out_dir, console, qemu_stdout, failure)
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
        if not args.keep_runtime:
            shutil.rmtree(runtime_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
