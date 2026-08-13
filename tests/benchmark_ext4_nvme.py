#!/usr/bin/env python3

import json
import os
import re
import selectors
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / ".artifacts"
OUT = ARTIFACTS / "test-results" / "ext4-nvme-benchmark"
BASE = OUT / "base.img"
ISO = ARTIFACTS / "m3.6b-linux-baseline" / "alpine-virt-3.22.5-x86_64.iso"
BOOT = ARTIFACTS / "limine-boot.img"
LINUX_GUEST = OUT / "linux-guest"
LINUX_BENCH = LINUX_GUEST / "ext4-nvme-benchmark.elf"
RUNS = int(os.environ.get("EXT4_NVME_BENCH_RUNS", "5"))
TIMEOUT = float(os.environ.get("EXT4_NVME_BENCH_TIMEOUT", "240"))

CPU = "qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+xsave,+avx,+avx2"
PHASES = (
    "cold_read",
    "write_sync",
    "overwrite_fsync",
    "create",
    "rename",
    "unlink",
    "syncfs",
)


def qemu_base() -> list[str]:
    return [
        "qemu-system-x86_64",
        "-machine", "q35",
        "-cpu", CPU,
        "-m", "2G",
        "-smp", "4",
        "-enable-kvm",
        "-monitor", "none",
        "-nographic",
        "-no-reboot",
        "-net", "none",
    ]


def make_overlay(name: str) -> Path:
    overlay = OUT / f"{name}.qcow2"
    overlay.unlink(missing_ok=True)
    subprocess.run(
        [
            "qemu-img", "create", "-q",
            "-f", "qcow2",
            "-F", "raw",
            "-b", str(BASE),
            str(overlay),
        ],
        cwd=ROOT,
        check=True,
    )
    return overlay


def terminate(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def read_until(
    process: subprocess.Popen[bytes],
    transcript: bytearray,
    marker: bytes,
    timeout: float,
) -> None:
    assert process.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout
    try:
        while marker not in transcript:
            if process.poll() is not None:
                raise RuntimeError(f"qemu exited status={process.returncode}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"timeout waiting for {marker!r}")
            for key, _ in selector.select(min(remaining, 1.0)):
                chunk = os.read(key.fd, 65536)
                if chunk:
                    transcript.extend(chunk)
    finally:
        selector.close()


def send(process: subprocess.Popen[bytes], line: str) -> None:
    assert process.stdin is not None
    process.stdin.write(line.encode() + b"\n")
    process.stdin.flush()


def run_pacha(run: int) -> str:
    overlay = make_overlay(f"pacha-{run}")
    log = OUT / f"pacha-{run}.log"
    command = qemu_base() + [
        "-drive", f"file={BOOT},format=raw,if=ide",
        "-drive", f"if=none,file={overlay},format=qcow2,id=rootdisk,cache=none,aio=native",
        "-device", "nvme,drive=rootdisk,serial=ext4-nvme-benchmark",
        "-boot", "order=c",
    ]
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    transcript = bytearray()
    try:
        read_until(process, transcript, b"KOBOX_EXT4_NVME_BENCH_DONE", TIMEOUT)
    finally:
        terminate(process)
        log.write_bytes(transcript)
        overlay.unlink(missing_ok=True)
    return transcript.decode(errors="replace")


def run_linux(run: int) -> str:
    overlay = make_overlay(f"linux-{run}")
    log = OUT / f"linux-{run}.log"
    command = qemu_base() + [
        "-cdrom", str(ISO),
        "-boot", "order=d",
        "-drive", f"if=none,file={overlay},format=qcow2,id=rootdisk,cache=none,aio=native",
        "-device", "nvme,drive=rootdisk,serial=ext4-nvme-benchmark",
        "-drive", f"file=fat:ro:{LINUX_GUEST},format=raw,if=virtio,readonly=on",
    ]
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    transcript = bytearray()
    try:
        read_until(process, transcript, b"localhost login:", 60)
        send(process, "root")
        read_until(process, transcript, b"localhost:~#", 20)
        send(
            process,
            "mkdir -p /pacha /bench; "
            "mount -o rw /dev/nvme0n1p2 /pacha; "
            "mount -o ro /dev/vda1 /bench; "
            "/bench/ext4-nvme-benchmark.elf /pacha",
        )
        read_until(process, transcript, b"LINUX_EXT4_NVME_BENCH_DONE", TIMEOUT)
        send(process, "sync; umount /pacha; poweroff")
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            terminate(process)
    finally:
        terminate(process)
        log.write_bytes(transcript)
        overlay.unlink(missing_ok=True)
    return transcript.decode(errors="replace")


def parse(text: str, prefix: str) -> dict[str, dict[str, int]]:
    results: dict[str, dict[str, int]] = {}
    pattern = re.compile(rf"^{prefix} phase=([a-z_]+) (.+?)\r?$", re.MULTILINE)
    for match in pattern.finditer(text):
        values: dict[str, int] = {}
        for token in match.group(2).split():
            key, separator, value = token.partition("=")
            if separator and value.isdigit():
                values[key] = int(value)
        results[match.group(1)] = values
    missing = [phase for phase in PHASES if phase not in results]
    if missing:
        raise RuntimeError(f"missing benchmark phases: {missing}")
    return results


def phase_ns(result: dict[str, dict[str, int]], phase: str) -> int:
    if phase in {"overwrite_fsync", "create", "rename", "unlink"}:
        return result[phase]["avg_ns"]
    return result[phase]["ns"]


def main() -> int:
    if RUNS < 1:
        raise SystemExit("EXT4_NVME_BENCH_RUNS must be positive")
    for required in (BASE, ISO, BOOT, LINUX_BENCH):
        if not required.exists():
            raise SystemExit(f"missing benchmark input: {required}")

    pacha_runs = []
    linux_runs = []
    for run in range(1, RUNS + 1):
        if run % 2 != 0:
            pacha_runs.append(parse(run_pacha(run), "KOBOX_EXT4_NVME_BENCH"))
            linux_runs.append(parse(run_linux(run), "LINUX_EXT4_NVME_BENCH"))
        else:
            linux_runs.append(parse(run_linux(run), "LINUX_EXT4_NVME_BENCH"))
            pacha_runs.append(parse(run_pacha(run), "KOBOX_EXT4_NVME_BENCH"))

    raw = {"pacha": pacha_runs, "linux": linux_runs}
    (OUT / "raw.json").write_text(json.dumps(raw, indent=2) + "\n")

    summary = {}
    for phase in PHASES:
        pacha_values = [phase_ns(run, phase) for run in pacha_runs]
        linux_values = [phase_ns(run, phase) for run in linux_runs]
        pacha_median = int(statistics.median(pacha_values))
        linux_median = int(statistics.median(linux_values))
        summary[phase] = {
            "linux_median_ns": linux_median,
            "pacha_median_ns": pacha_median,
            "overhead_ratio": pacha_median / linux_median,
            "linux_runs_ns": linux_values,
            "pacha_runs_ns": pacha_values,
        }
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    for phase in PHASES:
        row = summary[phase]
        print(
            f"EXT4_NVME_RESULT phase={phase} "
            f"linux_ns={row['linux_median_ns']} "
            f"pacha_ns={row['pacha_median_ns']} "
            f"overhead={row['overhead_ratio']:.3f}x"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
