#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot the new filed/storage ABI using immutable bootfs and private COW only.

The root backing disk is never opened writable by QEMU. Selected freshly
built service/runtime files are installed inside a newly allocated qcow2
overlay by Alpine, rather than synchronizing or copying the root filesystem.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

from benchmark_filed_baseline import Guest, overlay, qemu_common

ROOT = Path(__file__).resolve().parents[1]


class CutoverGuest(Guest):
    def record_chunk(self, source, chunk):
        self.log.write(chunk)
        self.log.flush()
        self.text.extend(chunk)
        # Serial diagnostics and virtconsole output can arrive between parts
        # of a shell prompt. Keep the latter independent for prompt matching.
        if source is not self.process.stdout:
            if not hasattr(self, 'console_text'):
                self.console_text = bytearray()
            self.console_text.extend(chunk)

    def drain_failure(self):
        # Service ready replies can wake the launcher before that service's
        # final failure diagnostic reaches serial. Retain this bounded tail
        # before terminating QEMU; it cannot turn a failed startup into PASS.
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline and self.process.poll() is None:
            for key, _ in self.selector.select(max(0, deadline - time.monotonic())):
                chunk = (os.read(key.fd, 65536) if key.fileobj is self.process.stdout
                         else key.fileobj.recv(65536))
                if not chunk:
                    self.selector.unregister(key.fileobj)
                    continue
                self.record_chunk(key.fileobj, chunk)

    def wait(self, marker, timeout=120, console_only=False):
        deadline = time.monotonic() + timeout
        while True:
            # Preserve the failure transcript and retire this guest promptly;
            # a dead filed service cannot make progress toward a shell prompt.
            if re.search(rb"\[(?:filed|storage_boot)\] (?:fatal\b|bootfs launch failed)|"
                         rb"\[seed0boot\] root ready failed|PACHA_KOBOX_FOUNDATION=FAIL",
                         self.text):
                self.drain_failure()
                raise RuntimeError("storage cutover startup failed; see transcript")
            if re.search(rb'(?:^|\n)vm: fault allocation failed|'
                         rb'(?:^|\n)USER fault principal=', self.text):
                self.drain_failure()
                raise RuntimeError('unhandled guest memory fault; see transcript')
            match_text = getattr(self, 'console_text', b'') if console_only else self.text
            if (marker.search(match_text) if isinstance(marker, re.Pattern)
                    else marker.encode() in match_text):
                return
            if self.process.poll() is not None:
                self.drain_failure()
                raise RuntimeError(f"QEMU exited: {self.process.returncode}, waiting for {marker}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"waiting for {marker}")
            for key, _ in self.selector.select(min(remaining, 1)):
                chunk = (os.read(key.fd, 65536) if key.fileobj is self.process.stdout
                         else key.fileobj.recv(65536))
                if not chunk:
                    self.selector.unregister(key.fileobj)
                    continue
                self.record_chunk(key.fileobj, chunk)
            if (b"FS_BENCH_FAIL" in self.text or
                    re.search(rb"(?:^|\n)FS_BASELINE_ERROR\r?\n", self.text)):
                self.drain_failure()
                raise RuntimeError("guest workload failed; see transcript")
            if re.search(rb"\[seed0root\] (?:rootfs service launch|Linux init launch|filed launch) failed status=", self.text):
                self.drain_failure()
                raise RuntimeError("PachaOS startup failed; see transcript")


def run(command, cwd=ROOT):
    subprocess.run([str(value) for value in command], cwd=cwd, check=True)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def install_spec(value):
    destination, separator, source = value.partition("=")
    if not separator or not re.fullmatch(r"/[A-Za-z0-9_./-]+", destination):
        raise argparse.ArgumentTypeError("expected absolute-guest-path=artifact-path")
    parts = destination.split("/")[1:]
    if any(part in ("", ".", "..") for part in parts):
        raise argparse.ArgumentTypeError("noncanonical install path")
    path = Path(source).resolve()
    if not path.is_file() or not path.stat().st_size:
        raise argparse.ArgumentTypeError(f"missing/empty artifact: {path}")
    return destination, path


def prepare(args, disk, installs):
    command = qemu_common(disk) + ["-cdrom", str(args.iso), "-boot", "order=d",
        "-drive", f"file=fat:ro:{args.out / 'guest'},format=raw,if=virtio,readonly=on"]
    (args.out / "prepare-command.json").write_text(json.dumps(command, indent=2) + "\n")
    guest = CutoverGuest(command, args.out / "prepare.log")
    try:
        guest.wait("localhost login:", args.timeout)
        guest.send("root")
        guest.wait("localhost:~#", args.timeout)
        operations = ["mkdir -p /pacha /artifacts",
            "mount -o rw,noatime /dev/nvme0n1p2 /pacha",
            "mount -o ro /dev/vda1 /artifacts"]
        guest.send("(" + " && ".join(operations) +
                   " && echo FILED_CUTOVER_MOUNTED) || echo FS_BASELINE_'ERROR'")
        guest.wait("\nFILED_CUTOVER_MOUNTED\r\n", args.timeout)
        for index, (destination, _) in enumerate(installs):
            operations = [
                f"mkdir -p {shlex.quote('/pacha' + str(Path(destination).parent))}",
                f"cp /artifacts/artifact{index:03d} {shlex.quote('/pacha' + destination)}",
                f"chmod 755 {shlex.quote('/pacha' + destination)}"]
            # Sealed network/GPU module closures can exceed a canonical tty
            # input line. Bound the transport per artifact, not filesystem ABI.
            marker = f"FILED_CUTOVER_INSTALLED_{index:03d}"
            guest.send("(" + " && ".join(operations) +
                       f" && echo {marker}) || echo FS_BASELINE_'ERROR'")
            guest.wait(f"\n{marker}\r\n", args.timeout)
        profile = "console-shell fs-storage-cutover" if args.native_smoke else "console-shell"
        operations = list(getattr(args, 'prepare_operations', ())) + [f"echo {shlex.quote(profile)} > /pacha/etc/pacha_boot_profile",
            "sync", "umount /pacha", "echo FILED_CUTOVER_PREPARED"]
        guest.send("(" + " && ".join(operations) + ") || echo FS_BASELINE_'ERROR'")
        guest.wait("\nFILED_CUTOVER_PREPARED\r\n", args.timeout)
        guest.send("poweroff")
        guest.process.wait(timeout=30)
    finally:
        guest.close()


def smoke(args, disk, boot_image):
    console = args.out / "console.sock"
    # QEMU's IDE disk model requires a writable block node even though Limine
    # only reads this image. Keep the staged boot image immutable as backing;
    # any firmware writes must land in a separate throwaway overlay.
    boot_overlay = args.out / "boot.qcow2"
    overlay(boot_overlay, boot_image, "raw")
    command = qemu_common(disk) + [
        "-drive", f"file={boot_overlay},format=qcow2,if=ide", "-boot", "order=c",
        "-chardev", f"socket,id=virtcon0,path={console},server=on,wait=off",
        "-device", "virtio-serial-pci,disable-legacy=on,iommu_platform=on,id=virtserial0",
        "-device", "virtconsole,chardev=virtcon0,name=org.pachaos.console.0"]
    if getattr(args, 'nvme_trace', None):
        command += ['-trace', f'events={args.nvme_trace.resolve()},file={args.out / "nvme-trace.log"}',
                    '-msg', 'timestamp=on']
    (args.out / "pacha-command.json").write_text(json.dumps(command, indent=2) + "\n")
    guest = CutoverGuest(command, args.out / "pacha.log")
    try:
        deadline = time.monotonic() + 15
        while not console.exists():
            if guest.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("console socket did not appear")
            time.sleep(0.05)
        guest.connect(console)
        if getattr(args, 'sandbox_bench_only', False):
            # The mount-time workload finishes before storage READY. Do not
            # start a client workload or wait for LPR/shell startup here.
            guest.wait(re.compile(rb'FS_INTERNAL_BENCH_DONE status=-?\d+\r?\n'), args.timeout)
            if not re.search(rb'FS_INTERNAL_BENCH_DONE status=0\r?\n', guest.text):
                raise RuntimeError('sandbox benchmark failed; see transcript')
            return guest.text.decode(errors='replace')
        if args.native_smoke:
            guest.wait("[seed0root] filed storage ABI smoke status=0", args.timeout)
            guest.wait("[seed0root] native public filesystem smoke status=0", args.timeout)
            guest.wait("[native-fs-smoke] child complete public-libc=ok", args.timeout)
        guest.wait("[termd] linux tty hvc open ready index=0 handle=", args.timeout)
        # Kernel banners also contain '# '. A shell prompt must terminate its
        # current console line; do not send the workload while init is starting.
        guest.wait(re.compile(rb"(?:^|\n)[^\r\n]*# (?=\r?\n|$)"), args.timeout)
        # A unique directory avoids relying on stale prior-run state. Shell
        # execution itself proves the new LPR image and exec ABI were used.
        guest.send("(mkdir /root/filed-cutover-smoke && "
            "printf 'storage-vq-smoke\\n' > /root/filed-cutover-smoke/a && "
            "test \"$(cat /root/filed-cutover-smoke/a)\" = storage-vq-smoke && "
            "ln /root/filed-cutover-smoke/a /root/filed-cutover-smoke/b && "
            "mv /root/filed-cutover-smoke/b /root/filed-cutover-smoke/c && "
            "test -f /root/filed-cutover-smoke/c && "
            "rm /root/filed-cutover-smoke/a /root/filed-cutover-smoke/c && "
            "rmdir /root/filed-cutover-smoke && sync && "
            "echo FILED_CUTOVER_SMOKE_OK) || echo FS_BASELINE_'ERROR'")
        guest.wait("\nFILED_CUTOVER_SMOKE_OK\r\n", args.timeout)
        if getattr(args, 'direct_smoke', False):
            guest.send("(mkdir /root/filed-direct-smoke && "
                "dd if=/dev/zero of=/root/filed-direct-smoke/a bs=1048576 count=2 && "
                "dd if=/root/filed-direct-smoke/a of=/root/filed-direct-smoke/b bs=1048576 && "
                "/bin/busybox cmp /root/filed-direct-smoke/a /root/filed-direct-smoke/b && "
                "/bin/busybox mknod /root/filed-direct-smoke/console c 5 1 && "
                "test \"$(/bin/busybox ls -1 /root/filed-direct-smoke)\" = \"$(printf 'a\\nb\\nconsole')\" && "
                "printf 'FILED_SERVICE_ALIAS_OK\\n' > /root/filed-direct-smoke/console && "
                "rm /root/filed-direct-smoke/a /root/filed-direct-smoke/b /root/filed-direct-smoke/console && "
                "rmdir /root/filed-direct-smoke && sync && "
                "echo FILED_DIRECT_SMOKE_OK) || echo FS_BASELINE_'ERROR'")
            guest.wait("\nFILED_SERVICE_ALIAS_OK\r\n", args.timeout)
            guest.wait("\nFILED_DIRECT_SMOKE_OK\r\n", args.timeout)
        if getattr(args, 'multiflight_smoke', False):
            guest.send("(/root/fs-multiflight-smoke /root && sync && "
                "echo FILED_MULTIFLIGHT_SMOKE_OK) || echo FS_BASELINE_'ERROR'")
            guest.wait("\nFS_BLOCKED_VFS_SMOKE_OK\r\n", args.timeout)
            guest.wait("\nFS_FORK_EXEC_LIFETIME_OK shared_ofd=ok parent_survives_child_exit=ok\r\n", args.timeout)
            guest.wait("\nFILED_MULTIFLIGHT_SMOKE_OK\r\n", args.timeout)
        return guest.text.decode(errors="replace")
    finally:
        # Preserve independent checkpoints even when a later unrelated service
        # prevents the shell. Child checks are not labelled child process exit.
        checkpoints = {
            "native_required": args.native_smoke,
            "raw_filesystem_process_exit": b"[seed0root] filed storage ABI smoke status=0" in guest.text,
            "public_native_parent_exit": b"[seed0root] native public filesystem smoke status=0" in guest.text,
            "public_native_exec_child_checks": b"[native-fs-smoke] child complete public-libc=ok" in guest.text,
            "lpr_shell_filesystem": b"\nFILED_CUTOVER_SMOKE_OK\r\n" in guest.text,
            "direct_required": getattr(args, 'direct_smoke', False),
            "direct_large_io_device_alias": b"\nFILED_DIRECT_SMOKE_OK\r\n" in guest.text,
            "multiflight_required": getattr(args, 'multiflight_smoke', False),
            "blocked_vfs_same_other_client": b"\nFS_BLOCKED_VFS_SMOKE_OK\r\n" in guest.text,
            "fork_exec_ofd_cloexec_exit": b"\nFS_FORK_EXEC_LIFETIME_OK shared_ofd=ok parent_survives_child_exit=ok\r\n" in guest.text,
            "multiflight_shared_offset_append": b"\nFILED_MULTIFLIGHT_SMOKE_OK\r\n" in guest.text,
        }
        if getattr(args, 'sandbox_bench_only', False):
            checkpoints = {'sandbox_benchmark_quiesced': bool(re.search(
                rb'FS_INTERNAL_BENCH_DONE status=0\r?\n', guest.text))}
        (args.out / "gates.json").write_text(json.dumps(checkpoints, indent=2) + "\n")
        guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--backing", type=Path, required=True,
        help="existing read-only console-profile qcow2 backing")
    parser.add_argument("--authority-disk", type=Path, default=ROOT / ".artifacts/disk.img",
        help="explicit configured raw pack target, opened read-only for root UUID")
    parser.add_argument("--boot-image", type=Path, default=ROOT / ".artifacts/limine-boot.img")
    parser.add_argument("--kernel", type=Path,
        help="fresh kernel ELF for the private boot image's KERNEL.ELF")
    parser.add_argument("--init-app", type=Path,
        help="fresh seed0boot ELF for the private boot image's INITAPP.ELF")
    parser.add_argument("--storage-boot", type=Path, required=True)
    parser.add_argument("--seed0root", type=Path, required=True)
    parser.add_argument("--filed", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--install", type=install_spec, action="append", default=[],
        help="selected new guest executable/runtime=host artifact, repeatable")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--stage-only", action="store_true",
        help="stage boot image/fixtures but do not run any guest")
    parser.add_argument("--sandbox-bench-only", action="store_true",
        help="boot existing prepared COW; stop after mount-time sandbox benchmark, no client workloads")
    parser.add_argument("--nvme-trace", type=Path,
        help="diagnostic QEMU event list; trace output is saved separately from the guest transcript")
    parser.add_argument("--native-smoke", action="store_true",
        help="run the explicitly installed native filed ABI fixture before shell startup")
    parser.add_argument("--direct-smoke", action="store_true",
        help="also exercise a resized direct queue and native tty device alias routing")
    parser.add_argument("--multiflight-smoke", action="store_true",
        help="exercise blocking VFS progress, threaded I/O, shared offsets and vector append")
    args = parser.parse_args()
    if args.sandbox_bench_only and (args.native_smoke or args.direct_smoke or args.multiflight_smoke):
        parser.error('sandbox benchmark cannot include client smoke workloads')
    if args.sandbox_bench_only and args.install:
        parser.error('sandbox benchmark uses the prepared backing; installing client artifacts is out of scope')
    args.out = args.out.resolve()
    # Prior overlays and logs are evidence; never reuse or delete them.
    args.out.mkdir(parents=True, exist_ok=False)
    for name in ("iso", "backing", "authority_disk", "boot_image", "storage_boot",
                 "seed0root", "filed", "package"):
        setattr(args, name, getattr(args, name).resolve())
    if args.init_app:
        args.init_app = args.init_app.resolve()
        if not args.init_app.is_file() or not args.init_app.stat().st_size:
            parser.error("init-app must be a nonempty seed0boot ELF")
    if args.kernel:
        args.kernel = args.kernel.resolve()
        if not args.kernel.is_file() or not args.kernel.stat().st_size:
            parser.error("kernel must be a nonempty kernel ELF")
    if not args.package.is_dir() or not (args.package / "manifest.bin").is_file():
        parser.error("package must be a sealed storage package directory")
    installs = args.install
    destinations = [destination for destination, _ in installs]
    if len(set(destinations)) != len(destinations):
        parser.error("duplicate install destination")
    if not args.stage_only and not args.sandbox_bench_only and "/lib/pacha/lpr-linux-x86_64.so" not in destinations:
        parser.error("a new LPR runtime must be explicitly installed")
    if args.native_smoke and "/sbin/filed_smoke.elf" not in destinations:
        parser.error("native-smoke requires an explicitly installed filed_smoke ELF")
    if args.native_smoke and "/sbin/native_fs_smoke.elf" not in destinations:
        parser.error("native-smoke requires an explicitly installed public libc FS/exec fixture")
    if args.native_smoke and {"/lib/libc.so", "/lib/ld-musl-x86_64.so.1"}.intersection(destinations):
        parser.error("native runtime must not replace Linux/LPR interpreter paths")
    native_runtime_paths = {"/usr/lib/pachaos/libc.so", "/usr/lib/pachaos/ld-musl-x86_64.so.1"}
    if args.native_smoke and not native_runtime_paths.issubset(destinations):
        parser.error("native-smoke requires the explicitly installed isolated native runtime")
    if args.multiflight_smoke and "/root/fs-multiflight-smoke" not in destinations:
        parser.error("multiflight-smoke requires an explicitly installed threaded fixture")
    guest_dir = args.out / "guest"
    guest_dir.mkdir()
    for index, (_, source) in enumerate(installs):
        shutil.copyfile(source, guest_dir / f"artifact{index:03d}")
    entries = [("/srv/storage_boot.elf", args.storage_boot),
        ("/sbin/seed0root.elf", args.seed0root), ("/sbin/filed.elf", args.filed)]
    for source in sorted(args.package.rglob("*")):
        if source.is_file():
            relative = source.relative_to(args.package).as_posix()
            if relative not in ("manifest.bin", "core.so", "sandbox.elf") and not (
                    relative.startswith("modules/") and relative.endswith(".ko")):
                raise RuntimeError(f"unexpected build helper in sealed package: {relative}")
            entries.append(("/srv/kobox2/storage/" + relative, source))
    # Parallel builds can replace their output paths. Freeze only the selected
    # boot/service artifacts, so the manifest and hashes describe bytes this
    # run actually boots, not a later build at the same source path.
    bootfs_inputs = args.out / "bootfs-inputs"
    bootfs_inputs.mkdir()
    staged_entries = []
    for index, (destination, source) in enumerate(entries):
        staged = bootfs_inputs / f"artifact{index:03d}"
        shutil.copyfile(source, staged)
        staged_entries.append((destination, staged))
    manifest = args.out / "bootfs.manifest"
    manifest.write_text("".join(f"{destination}={source}\n"
                              for destination, source in staged_entries))
    archive = args.out / "BOOTFS.IMG"
    run(["go", "run", "./cmd/bootfs-stage", "-manifest", manifest, "-output", archive,
         "-disk", args.authority_disk], ROOT / "pack")
    # This is a 64MiB bootloader/FAT artifact, not the root disk. The staged
    # copy is modified offline once, then serves as immutable COW backing.
    boot_template_sha256 = digest(args.boot_image)
    boot_image = args.out / "limine-boot.img"
    shutil.copyfile(args.boot_image, boot_image)
    run(["mcopy", "-o", "-i", f"{boot_image}@@2097152", archive, "::/BOOTFS.IMG"])
    if args.kernel:
        # The boot template can contain an older kernel than the selected own
        # bootstrap ABI. Freeze this exact producer alongside the init app;
        # modifying the private FAT image must never modify the template.
        kernel = args.out / "KERNEL.ELF"
        shutil.copyfile(args.kernel, kernel)
        run(["mcopy", "-o", "-i", f"{boot_image}@@2097152", kernel, "::/KERNEL.ELF"])
    if args.init_app:
        init_app = args.out / "INITAPP.ELF"
        shutil.copyfile(args.init_app, init_app)
        run(["mcopy", "-o", "-i", f"{boot_image}@@2097152", init_app, "::/INITAPP.ELF"])
    metadata = {"backing": str(args.backing), "backing_sha256": digest(args.backing),
        "native_smoke": args.native_smoke, "direct_smoke": args.direct_smoke,
        "sandbox_bench_only": args.sandbox_bench_only,
        "authority_disk": str(args.authority_disk),
        "boot_template": str(args.boot_image),
        "boot_template_sha256": boot_template_sha256,
        "boot_image_sha256": digest(boot_image), "bootfs_sha256": digest(archive),
        "kernel": ({"source": str(args.kernel), "sha256": digest(kernel)}
                   if args.kernel else None),
        "init_app": ({"source": str(args.init_app), "sha256": digest(init_app)}
                     if args.init_app else None),
        "bootfs_inputs": [{"path": destination, "source": str(source),
                           "sha256": digest(staged_entries[index][1])}
                          for index, (destination, source) in enumerate(entries)],
        "installs": [{"path": destination, "source": str(source),
                      "sha256": digest(guest_dir / f"artifact{index:03d}")}
                     for index, (destination, source) in enumerate(installs)],
        "write_policy": "new qcow2 overlays only; selected binaries, no rootfs sync/copy"}
    (args.out / "inputs.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if args.stage_only:
        print(f"FILED_CUTOVER_STAGED {args.out}")
        return
    prepared = args.out / "prepared.qcow2"
    overlay(prepared, args.backing, "qcow2")
    try:
        if not args.sandbox_bench_only:
            prepare(args, prepared, installs)
        pacha_disk = args.out / "pacha.qcow2"
        overlay(pacha_disk, prepared, "qcow2")
        smoke(args, pacha_disk, boot_image)
    finally:
        # A failed boot is still a run against the user's immutable backing;
        # verify that invariant even when no success result can be emitted.
        unchanged = digest(args.backing) == metadata["backing_sha256"]
        template_unchanged = digest(args.boot_image) == boot_template_sha256
        (args.out / "backing-check.json").write_text(
            json.dumps({"backing_unchanged": unchanged,
                        "boot_template_unchanged": template_unchanged}, indent=2) + "\n")
        if not unchanged:
            raise RuntimeError("read-only backing changed")
        if not template_unchanged:
            raise RuntimeError("read-only boot template changed")
    (args.out / "result.json").write_text(json.dumps({"ok": True,
        "marker": "FS_INTERNAL_BENCH_DONE" if args.sandbox_bench_only else "FILED_CUTOVER_SMOKE_OK", "native_smoke": args.native_smoke,
        "direct_smoke": args.direct_smoke,
        "multiflight_smoke": args.multiflight_smoke,
        "backing_unchanged": True}, indent=2) + "\n")
    print(f"{'FS_SANDBOX_ONLY_OK' if args.sandbox_bench_only else 'FILED_CUTOVER_SMOKE_OK'} {args.out}")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FILED_CUTOVER_ERROR: {error}", file=sys.stderr)
        raise
