#!/usr/bin/env python3
import argparse
import os
import random
import re
import shlex
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
THREAD_PREFIX_RE = re.compile(r"\[Thread [0-9]+\]\s*")
BAD_SERIAL_MARKERS = (
    "PAGE FAULT",
    "#PF",
    "GENERAL PROTECTION",
    "INVALID OPCODE",
    "STACK SEGMENT FAULT",
    "SEGMENT NOT PRESENT",
    "ExecLoader: child faulted",
)
CACHEABLE_PREFIXES = ("/lib/", "/bin/", "/cmd/", "/sbin/", "/usr/bin/", "/usr/lib/")
CACHE_COUNTERS = (
    "LinuxAbiServer.perf.cache.file_hits",
    "LinuxAbiServer.perf.cache.file_misses",
    "LinuxAbiServer.perf.cache.file_fill_bytes",
    "LinuxAbiServer.perf.cache.file_evictions",
    "LinuxAbiServer.perf.cache.file_reuse_bytes",
    "LinuxAbiServer.perf.cache.file_fill_fail_slot",
    "LinuxAbiServer.perf.cache.file_fill_fail_alloc",
    "LinuxAbiServer.perf.cache.file_fill_fail_read",
    "LinuxAbiServer.perf.fs.read_bytes",
    "LinuxAbiServer.perf.fs.read_cmd_bytes",
    "LinuxAbiServer.perf.fs.read_lib_bytes",
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
        "virtio-net-pci,netdev=capnet0,mac=52:54:00:12:34:78",
        "-serial",
        f"file:{run_dir / 'serial.log'}",
        "-device",
        "virtio-serial-pci",
        "-chardev",
        "pty,id=capconsole",
        "-device",
        "virtconsole,chardev=capconsole,name=capabilityos.console.0",
    ]


def make_command(index: int, body: str) -> tuple[bytes, bytes, str]:
    begin = f"__FILE_BEGIN_{index:04d}__"
    end = f"__FILE_END_{index:04d}__"
    wrapped = f"printf '{begin}\\n'; {body}; rc=$?; printf '\\n{end}:%s\\n' \"$rc\""
    return begin.encode("ascii"), end.encode("ascii"), wrapped


def run_shell_command(console: Console, index: int, label: str, body: str, timeout: float, send_delay: float) -> tuple[bytes, dict[str, float]]:
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
    if b":0" not in plain[: plain.find(b"# ") if b"# " in plain else len(plain)]:
        raise RuntimeError(f"{label} non-zero or missing rc: {plain!r}")
    return plain, {
        "write_s": wrote_at - sent_at,
        "echo_s": echo_at - wrote_at,
        "begin_s": begin_at - echo_at,
        "command_s": end_at - begin_at,
        "prompt_s": prompt_at - end_at,
        "total_s": prompt_at - sent_at,
    }


def payload_between_markers(index: int, out: bytes) -> bytes:
    begin = f"__FILE_BEGIN_{index:04d}__".encode("ascii")
    end = f"__FILE_END_{index:04d}__".encode("ascii")
    body_start = out.rfind(begin)
    body_end = out.rfind(end)
    if body_start < 0 or body_end <= body_start:
        return out
    return out[body_start + len(begin) : body_end]


def collect_guest_paths(console: Console, timeout: float, send_delay: float) -> list[str]:
    body = "for d in /bin /usr/bin /usr/lib /lib /cmd /sbin; do for f in \"$d\"/*; do [ -f \"$f\" ] && printf '%s\\n' \"$f\"; done; done"
    out, _ = run_shell_command(console, 0, "collect-paths", body, timeout, send_delay)
    payload = payload_between_markers(0, out)
    paths: list[str] = []
    seen: set[str] = set()
    for raw in payload.decode("utf-8", errors="ignore").splitlines():
        path = raw.strip()
        if not path.startswith(CACHEABLE_PREFIXES):
            continue
        if any(ch.isspace() for ch in path):
            continue
        if path in seen:
            continue
        seen.add(path)
        paths.append(path)
    return paths


def file_read_body(path: str, read_bytes: int) -> str:
    quoted = shlex.quote(path)
    return f"CAPABILITYOS_EXEC_PROFILE=1 CAPABILITYOS_EXEC_PROFILE_DETAIL=1 head -c {read_bytes} {quoted} | wc -c"


def check_serial(runtime_dir: Path) -> None:
    serial_path = runtime_dir / "serial.log"
    if not serial_path.exists():
        return
    serial = serial_path.read_text(encoding="utf-8", errors="replace")
    for marker in BAD_SERIAL_MARKERS:
        if marker in serial:
            raise RuntimeError(f"serial contains {marker!r}")


def normalize_serial(text: str) -> str:
    return THREAD_PREFIX_RE.sub("", text)


def parse_counter_sums(serial: str) -> dict[str, int]:
    clean = normalize_serial(serial)
    counters: dict[str, int] = {}
    for key in CACHE_COUNTERS:
        values = [int(match.group(1)) for match in re.finditer(re.escape(key) + r"=([0-9]+)", clean)]
        counters[key] = sum(values)
    return counters


def write_results(out_dir: Path, completed: list[str], timings: list[tuple[int, str, dict[str, float]]], result: str, counters: dict[str, int] | None) -> None:
    timing_lines = ["index\tlabel\twrite_s\techo_s\tbegin_s\tcommand_s\tprompt_s\ttotal_s"]
    for item_index, item_label, timing in timings:
        timing_lines.append(
            f"{item_index}\t{item_label}\t{timing['write_s']:.6f}\t{timing['echo_s']:.6f}\t{timing['begin_s']:.6f}\t{timing['command_s']:.6f}\t{timing['prompt_s']:.6f}\t{timing['total_s']:.6f}"
        )
    (out_dir / "timings.tsv").write_text("\n".join(timing_lines) + "\n", encoding="utf-8")
    summary = list(completed)
    if counters is not None:
        summary.append("cache counters:")
        for key in CACHE_COUNTERS:
            summary.append(f"{key}={counters.get(key, 0)}")
    summary.append(result)
    (out_dir / "summary.txt").write_text("\n".join(summary) + "\n", encoding="utf-8")


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Stress Linux ABI readonly file reads and file-cache counters through the virtio console.")
    parser.add_argument("--passes", type=int, default=2, help="number of passes over the selected guest paths")
    parser.add_argument("--max-files", type=int, default=96, help="maximum distinct guest paths to read")
    parser.add_argument("--min-files", type=int, default=70, help="fail if fewer cacheable guest files are found")
    parser.add_argument("--read-bytes", type=int, default=4096)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--out", default=".artifacts/file-stress")
    parser.add_argument("--smp", type=int, default=4)
    parser.add_argument("--send-delay", type=float, default=0.03)
    parser.add_argument("--require-file-cache", action="store_true", help="fail if file cache fills/evictions are not observed")
    parser.add_argument("--keep-runtime", action="store_true")
    args = parser.parse_args()

    if os.name == "nt":
        raise SystemExit("run this under WSL/Linux: wsl -e bash -lc 'cd /mnt/c/.../CapabilityOS && python3 tools/file_stress.py'")
    if args.passes < 1:
        parser.error("--passes must be positive")
    if args.max_files < 1:
        parser.error("--max-files must be positive")
    if args.min_files < 1:
        parser.error("--min-files must be positive")
    if args.read_bytes < 1:
        parser.error("--read-bytes must be positive")

    random.seed(args.seed)
    root = Path.cwd()
    out_dir = root / args.out
    runtime_dir = Path("/tmp") / f"capabilityos-file-stress-{os.getpid()}"
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
    counters: dict[str, int] | None = None
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

        paths = collect_guest_paths(console, args.timeout, args.send_delay)
        if len(paths) < args.min_files:
            raise RuntimeError(f"found only {len(paths)} cacheable files; need at least {args.min_files}")
        random.shuffle(paths)
        paths = paths[: args.max_files]
        (out_dir / "paths.txt").write_text("\n".join(paths) + "\n", encoding="utf-8")
        print(f"selected {len(paths)} guest files", flush=True)

        index = 1
        for pass_index in range(1, args.passes + 1):
            for path in paths:
                label = f"read-pass{pass_index}"
                body = file_read_body(path, args.read_bytes)
                print(f"{index:04d} {label}: {path}", flush=True)
                out, timing = run_shell_command(console, index, label, body, args.timeout, args.send_delay)
                payload = payload_between_markers(index, out)
                if not re.search(rb"\b[0-9]+\b", payload):
                    raise RuntimeError(f"{label} missing byte count for {path}: {out!r}")
                timings.append((index, label, timing))
                completed.append(f"{index:04d} {label} ok path={path} total={timing['total_s']:.3f}s command={timing['command_s']:.3f}s")
                print(completed[-1], flush=True)
                (out_dir / "last-output.txt").write_bytes(out)
                check_serial(runtime_dir)
                index += 1

        serial_path = runtime_dir / "serial.log"
        serial_text = serial_path.read_text(encoding="utf-8", errors="replace") if serial_path.exists() else ""
        counters = parse_counter_sums(serial_text)
        if counters.get("LinuxAbiServer.perf.cache.file_fill_fail_slot", 0) != 0:
            raise RuntimeError("file cache slot fill failures were reported")
        if counters.get("LinuxAbiServer.perf.cache.file_fill_fail_alloc", 0) != 0:
            raise RuntimeError("file cache allocation failures were reported")
        if counters.get("LinuxAbiServer.perf.cache.file_fill_fail_read", 0) != 0:
            raise RuntimeError("file cache read failures were reported")
        if args.require_file_cache:
            if counters.get("LinuxAbiServer.perf.cache.file_misses", 0) == 0 and counters.get("LinuxAbiServer.perf.cache.file_fill_bytes", 0) == 0:
                raise RuntimeError("file cache was not exercised; read cache appears inactive")
            if len(paths) > 64 and counters.get("LinuxAbiServer.perf.cache.file_evictions", 0) == 0:
                raise RuntimeError("file cache eviction was not observed")

        result = f"result: ok ({len(completed)} reads, {len(paths)} files, {args.passes} passes)"
        write_results(out_dir, completed, timings, result, counters)
        for key in CACHE_COUNTERS:
            print(f"{key}={counters.get(key, 0)}", flush=True)
        print(result, flush=True)
        return 0
    except Exception as exc:
        failure = f"failed after {len(completed)} reads: {exc}"
        print(failure, flush=True)
        write_results(out_dir, completed, timings, failure, counters)
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
