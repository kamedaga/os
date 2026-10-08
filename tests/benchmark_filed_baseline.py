#!/usr/bin/env python3
"""Run the identical static musl workload through filed and native Alpine VFS.

The existing root disk is a read-only backing file. Preparation and both
guests write only qcow2 overlays; no rootfs is copied or synchronized.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import socket
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
PHASES = {
    *(f"{op}_d{depth}" for op in ("stat", "open_close", "readdir_256", "create_close", "rename", "unlink")
      for depth in (0, 4, 8)),
    "stat_missing", "readdir_256", "create_unlink", "mkdir_rmdir", "rename_pair",
    "pread_4k_hot", "read_1m_hot", "pwrite_4k", "pwrite_fsync", "mmap_1m_touch",
}
RESULT = re.compile(r"^FS_BENCH fs=(\S+) phase=(\S+) ops=(\d+) total_ns=(\d+) ns_per_op=(\d+)\r?$", re.M)


def run(command):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, check=True)


def overlay(path, backing, backing_format):
    # Refuse reuse: silently deleting a prior overlay would lose diagnostic
    # evidence and allow a partial run to masquerade as a fresh baseline.
    if path.exists():
        raise RuntimeError(f"output already exists: {path}")
    run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", backing_format, "-b", backing, path])


class Guest:
    def __init__(self, command, log):
        self.log = log.open("wb")
        self.process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.text = bytearray()
        self.console = None

    def wait(self, marker, timeout=120):
        deadline = time.monotonic() + timeout
        while marker.encode() not in self.text:
            if self.process.poll() is not None:
                raise RuntimeError(f"QEMU exited: {self.process.returncode}, waiting for {marker}")
            if time.monotonic() >= deadline:
                raise TimeoutError(f"waiting for {marker}")
            for key, _ in self.selector.select(1):
                chunk = os.read(key.fd, 65536) if key.fileobj is self.process.stdout else key.fileobj.recv(65536)
                if not chunk:
                    self.selector.unregister(key.fileobj)
                    continue
                self.log.write(chunk)
                self.log.flush()
                self.text.extend(chunk)
            if b"FS_BENCH_FAIL" in self.text or re.search(rb"(?:^|\n)FS_BASELINE_ERROR\r?\n", self.text):
                raise RuntimeError("guest workload failed; see transcript")
            if re.search(rb"\[seed0root\] (?:rootfs service launch|Linux init launch|filed launch) failed status=", self.text):
                raise RuntimeError("PachaOS startup failed; see transcript")

    def send(self, command):
        data = (command + "\n").encode()
        if self.console:
            self.console.sendall(data)
        else:
            self.process.stdin.write(data)
            self.process.stdin.flush()

    def connect(self, path):
        self.console = socket.socket(socket.AF_UNIX)
        self.console.connect(str(path))
        self.selector.register(self.console, selectors.EVENT_READ)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.selector.close()
        if self.console:
            self.console.close()
        self.log.close()


def qemu_common(disk):
    return ["qemu-system-x86_64", "-machine", "q35", "-cpu", "host", "-m", "4G",
            "-smp", "4", "-enable-kvm", "-monitor", "none", "-display", "none",
            "-serial", "stdio", "-no-reboot", "-net", "none",
            # The installed desktop init requests netd's client endpoint even
            # for a console-only session; an absent NIC makes exec fail with
            # ENOTCONN before the benchmark can start. No host ports are used.
            "-netdev", "user,id=net0",
            "-device", "virtio-net-pci,netdev=net0,disable-legacy=on,iommu_platform=on,csum=off,gso=off,guest_csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,mrg_rxbuf=off",
            "-device", "intel-iommu,intremap=off,aw-bits=48",
            "-drive", f"if=none,file={disk},format=qcow2,id=rootdisk,cache=none,aio=native",
            "-device", "nvme,drive=rootdisk,serial=capos-root",
            "-device", "virtio-gpu-pci,disable-legacy=on,iommu_platform=on,id=pachagpu",
            "-device", "virtio-keyboard-pci,disable-legacy=on,iommu_platform=on,id=pachakbd",
            "-device", "virtio-mouse-pci,disable-legacy=on,iommu_platform=on,id=pachamouse"]


def linux(args, disk, role):
    cmd = qemu_common(disk) + ["-cdrom", str(args.iso), "-boot", "order=d",
                             "-drive", f"file=fat:ro:{args.out / 'guest'},format=raw,if=virtio,readonly=on"]
    guest = Guest(cmd, args.out / f"{role}.log")
    (args.out / f"{role}-command.json").write_text(json.dumps(cmd, indent=2) + "\n")
    try:
        guest.wait("localhost login:")
        guest.send("root")
        guest.wait("localhost:~#")
        command = (
            "mkdir -p /pacha /bench; "
            "mount -o rw,noatime /dev/nvme0n1p2 /pacha && "
            "mount -o ro /dev/vda1 /bench && "
            "cp /bench/bench.elf /pacha/cmd/lpr_fs_microbench.elf && "
            "cp /bench/bench.sh /pacha/cmd/lpr_fs_microbench.sh && "
            "chmod 755 /pacha/cmd/lpr_fs_microbench.elf && "
            "echo console-shell > /pacha/etc/pacha_boot_profile"
        )
        if role == "prepare":
            # The legacy static GPU discovery fixture conflicts with the
            # current gpud publisher (EEXIST) and prevents the console from
            # starting. Remove only its six entries in the private overlay;
            # gpud publishes them from its actual device grant at boot.
            command += (
                " && ls -ld /pacha/dev/dri/card0 /pacha/dev/dri/renderD128"
                " /pacha/sys/class/drm/card0 /pacha/sys/class/drm/renderD128"
                " /pacha/sys/dev/char/226:0 /pacha/sys/dev/char/226:128"
                " && rm -f /pacha/dev/dri/card0 /pacha/dev/dri/renderD128"
                " /pacha/sys/class/drm/card0 /pacha/sys/class/drm/renderD128"
                " /pacha/sys/dev/char/226:0 /pacha/sys/dev/char/226:128"
            )
            command += " && sync && umount /pacha && echo FS_BASELINE_PREPARED"
            marker = "FS_BASELINE_PREPARED"
        else:
            # A chroot preserves the exact absolute paths used by LPR. An
            # explicit mount prevents Alpine /tmp on the initramfs (ramfs)
            # from being incorrectly compared with filed's tmpfs.
            command += (
                " && mount -t tmpfs -o size=512m tmpfs /pacha/tmp"
                " && uname -a && cat /etc/alpine-release && cat /proc/mounts"
                " && sha256sum /pacha/cmd/lpr_fs_microbench.elf"
                f" && for trial in $(seq 1 {args.trials}); do"
                " echo FS_MICROBENCH_TRIAL trial=$trial;"
                f" chroot /pacha /cmd/lpr_fs_microbench.elf ext4 /root {args.scale} || exit 1;"
                f" chroot /pacha /cmd/lpr_fs_microbench.elf tmpfs /tmp {args.scale} || exit 1;"
                " done"
                " && sync && umount /pacha/tmp && umount /pacha && echo FS_BASELINE_LINUX_OK"
            )
            marker = "FS_BASELINE_LINUX_OK"
        guest.send(f"({command}) || echo FS_BASELINE_ERROR")
        guest.wait(marker, args.timeout)
        # The completion markers also occur in echoed commands; require the
        # separate emitted line before accepting a completed preparation/run.
        guest.wait("\n" + marker + "\r\n", args.timeout)
        guest.send("poweroff")
        guest.process.wait(timeout=30)
        return guest.text.decode(errors="replace")
    finally:
        guest.close()


def pacha(args, disk):
    path = args.out / "console.sock"
    cmd = qemu_common(disk) + ["-drive", f"file={ROOT / '.artifacts/limine-boot.img'},format=raw,if=ide",
                             "-boot", "order=c",
                             "-chardev", f"socket,id=virtcon0,path={path},server=on,wait=off",
                             "-device", "virtio-serial-pci,disable-legacy=on,iommu_platform=on,id=virtserial0",
                             "-device", "virtconsole,chardev=virtcon0,name=org.pachaos.console.0"]
    guest = Guest(cmd, args.out / "pacha.log")
    (args.out / "pacha-command.json").write_text(json.dumps(cmd, indent=2) + "\n")
    try:
        deadline = time.monotonic() + 10
        while not path.exists():
            if guest.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("console socket did not appear")
            time.sleep(0.05)
        guest.connect(path)
        guest.wait("[termd] linux tty hvc open ready index=0 handle=", args.timeout)
        guest.wait("# ", 120)
        guest.send(f"sh /cmd/lpr_fs_microbench.sh {args.scale} {args.trials}")
        guest.wait("\nFS_MICROBENCH_DONE\r\n", args.timeout)
        guest.send("sync")
        return guest.text.decode(errors="replace")
    finally:
        guest.close()


def parse(text, trials):
    if "FS_BENCH_FAIL" in text:
        raise RuntimeError("benchmark failure")
    results = {}
    for fs, phase, ops, total, average in RESULT.findall(text):
        row = {"ops": int(ops), "total_ns": int(total), "ns_per_op": int(average)}
        if row["ops"] <= 0 or row["total_ns"] < 1000000000 or row["ns_per_op"] != row["total_ns"] // row["ops"]:
            raise RuntimeError(f"invalid/short measurement: {fs} {phase} {row}")
        results.setdefault(fs, {}).setdefault(phase, []).append(row)
    for fs in ("ext4", "tmpfs"):
        if set(results.get(fs, {})) != PHASES:
            raise RuntimeError(f"incomplete {fs} phases")
        if text.count(f"FS_BENCH_DONE fs={fs}") != trials:
            raise RuntimeError(f"incomplete {fs} runs")
        for phase, rows in results[fs].items():
            if len(rows) != trials:
                raise RuntimeError(f"incomplete {fs} {phase} trials")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--scale", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    if args.trials < 1 or not 1 <= args.scale <= 1000:
        parser.error("invalid trials or scale")
    args.out = args.out.resolve()
    args.iso = args.iso.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    fixtures = args.out / "guest"
    fixtures.mkdir()
    binary = ROOT / ".artifacts/userland-fixtures/lpr_fs_microbench.static.elf"
    run(["cp", binary, fixtures / "bench.elf"])
    run(["cp", ROOT / "tests/fixtures/lpr_fs_microbench.sh", fixtures / "bench.sh"])
    run(["cp", ROOT / "userland/fixtures/src/wsl_musl/lpr_fs_microbench.c", args.out / "benchmark-source.c"])
    prepared = args.out / "prepared.qcow2"
    overlay(prepared, ROOT / ".artifacts/disk.img", "raw")
    metadata = {
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "source_sha256": hashlib.sha256((ROOT / "userland/fixtures/src/wsl_musl/lpr_fs_microbench.c").read_bytes()).hexdigest(),
        "iso_sha256": hashlib.sha256(args.iso.read_bytes()).hexdigest(),
        "boot_image_sha256": hashlib.sha256((ROOT / ".artifacts/limine-boot.img").read_bytes()).hexdigest(),
        "trials": args.trials, "scale": args.scale, "minimum_phase_ns": 1000000000,
        "qemu": subprocess.check_output(["qemu-system-x86_64", "--version"], text=True).splitlines()[0],
        "shared_qemu_args": qemu_common(prepared),
        "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "git_status": subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True),
        "host": os.uname()._asdict() if hasattr(os.uname(), "_asdict") else list(os.uname()),
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "preparation": "benchmark binary/script, console-shell profile, remove six legacy GPU discovery entries in COW only",
    }
    (args.out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("Preparing COW root disk with the benchmark and console profile", flush=True)
    linux(args, prepared, "prepare")
    native_disk, pacha_disk = args.out / "linux.qcow2", args.out / "pacha.qcow2"
    overlay(native_disk, prepared, "qcow2")
    overlay(pacha_disk, prepared, "qcow2")
    print("Measuring LPR -> filed ext4/tmpfs", flush=True)
    pacha_text = pacha(args, pacha_disk)
    current = parse(pacha_text, args.trials)
    (args.out / "pacha-raw.json").write_text(json.dumps(current, indent=2) + "\n")
    print("Measuring native Alpine ext4/tmpfs", flush=True)
    native_text = linux(args, native_disk, "linux")
    native = parse(native_text, args.trials)
    (args.out / "linux-raw.json").write_text(json.dumps(native, indent=2) + "\n")
    frequencies = {os_name: [int(value) for value in re.findall(r"tsc_hz=(\d+)", text)]
                   for os_name, text in (("linux", native_text), ("pacha", pacha_text))}
    if any(len(values) != 2 * args.trials for values in frequencies.values()):
        raise RuntimeError("missing clock calibration")
    all_hz = frequencies["linux"] + frequencies["pacha"]
    if max(all_hz) / min(all_hz) > 1.01:
        raise RuntimeError(f"guest clock calibration disagrees: {frequencies}")
    raw = {"linux": native, "pacha": current, "tsc_hz": frequencies}
    (args.out / "raw.json").write_text(json.dumps(raw, indent=2) + "\n")
    summary = {}
    for fs in ("ext4", "tmpfs"):
        summary[fs] = {}
        for phase in sorted(PHASES):
            p = int(statistics.median(row["ns_per_op"] for row in current[fs][phase]))
            l = int(statistics.median(row["ns_per_op"] for row in native[fs][phase]))
            summary[fs][phase] = {"pacha_median_ns": p, "linux_median_ns": l, "ratio": p / l}
            print(f"FS_BASELINE fs={fs} phase={phase} pacha_ns={p} linux_ns={l} ratio={p/l:.3f}x")
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
