#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Functional device coverage for the shared fiber adapter, never an FS benchmark."""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

from benchmark_filed_baseline import overlay
from qemu_filed_cutover_smoke import digest, prepare
from qemu_xfce_gui_perf import VM, type_text
from validate_filed_nvme import linux_script

ROOT = Path(__file__).resolve().parents[1]


class USBKeys:
    """Leave distinct press/release reports for an interrupt-polled USB HID."""
    def __init__(self, qmp):
        self.qmp = qmp

    def chord(self, *codes):
        self.qmp.execute('input-send-event', {'events': [self.qmp.key_event(code, True) for code in codes]})
        time.sleep(.06)
        self.qmp.execute('input-send-event', {'events': [self.qmp.key_event(code, False) for code in reversed(codes)]})
        time.sleep(.06)

    def key(self, code):
        self.chord(code)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--adapters', type=Path, required=True)
    parser.add_argument('--usb-package', type=Path, required=True)
    parser.add_argument('--nvme-package', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--memory', default='4G')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--storage-package', type=Path)
    parser.add_argument('--observe-seconds', type=int, default=0)
    parser.add_argument('--keep-running', action='store_true',
                        help='Keep the GUI alive on both success and failure until explicitly stopped')
    parser.add_argument('--interactive', action='store_true',
                        help='Leave keyboard/mouse to the user after desktop acceptance')
    args = parser.parse_args()
    if args.interactive and not args.keep_running:
        parser.error('--interactive requires --keep-running')
    out, adapters = args.out.resolve(), args.adapters.resolve()
    out.mkdir(parents=True, exist_ok=False)
    base = ROOT / '.artifacts/fs-sandbox-a-clock-page/base1/prepared.qcow2'
    boot_source = ROOT / '.artifacts/fs-worker-publish/normal2/qemu'
    iso = ROOT / '.artifacts/fs-bench/baseline-20261002/alpine-virt-3.22.5-x86_64.iso'
    installs = [('/srv/kobox2/sandbox.elf', adapters/'gpu.elf'),
                ('/srv/kobox2/network/sandbox.elf', adapters/'network.elf'),
                ('/srv/kobox2/usb-storage/sandbox.elf', adapters/'usb.elf'),
                ('/srv/kobox2/nvme/sandbox.elf', adapters/'nvme.elf'),
                ('/cmd/xfce_app_acceptance.sh', ROOT/'tests/run-lpr-qemu-xfce-app-acceptance.sh')]
    for profile, package in (('usb-storage', args.usb_package), ('nvme', args.nvme_package)):
        package = package.resolve()
        files = [package/'manifest.bin', package/'core.so', *sorted((package/'modules').glob('*.ko'))]
        if not files[0].is_file() or not files[1].is_file() or len(files) == 2:
            raise ValueError(f'incomplete sealed package: {package}')
        installs += [(f'/srv/kobox2/{profile}/{source.relative_to(package)}', source) for source in files]
    guest_dir = out / 'guest'
    guest_dir.mkdir()
    for index, (_, source) in enumerate(installs):
        shutil.copyfile(source, guest_dir/f'artifact{index:03d}')
    fsck_inputs = ROOT/'.artifacts/fs-worker-publish/normal2/guest'
    for source in fsck_inputs.glob('*.apk'):
        shutil.copyfile(source, guest_dir/source.name)
    shutil.copyfile(ROOT/'tests/fixtures/filed_nvme_fsck.sh', guest_dir/'fsck.sh')
    disk = out / 'devices.qcow2'
    overlay(disk, base, 'qcow2')
    # Only small boot artifacts and named adapter executables are staged;
    # neither the installed root nor its read-only backing is copied/synced.
    boot = out / 'limine-boot.img'
    shutil.copyfile(boot_source/'limine-boot.img', boot)
    entries = []
    for line in (boot_source/'bootfs.manifest').read_text().splitlines():
        destination, source = line.split('=', 1)
        selected = adapters/'storage.elf' if destination.endswith('/storage/sandbox.elf') else Path(source)
        storage_prefix = '/srv/kobox2/storage/'
        if (args.storage_package and destination.startswith(storage_prefix) and
                not destination.endswith('/sandbox.elf')):
            selected = args.storage_package.resolve()/destination[len(storage_prefix):]
        frozen = out / f'boot-{len(entries):03d}'
        shutil.copyfile(selected, frozen)
        entries.append((destination, frozen))
    manifest = out/'bootfs.manifest'
    manifest.write_text(''.join(f'{name}={source}\n' for name, source in entries))
    archive = out/'BOOTFS.IMG'
    subprocess.run(['go', 'run', './cmd/bootfs-stage', '-manifest', str(manifest),
                    '-output', str(archive), '-disk', str(ROOT/'.artifacts/disk.img')],
                   cwd=ROOT/'pack', check=True)
    # Rebuild only our private FAT volume: repeated archive replacement leaves
    # wrapped chains which the BIOS loader failed to open in device-run1/2.
    boot_files = out/'boot-files'
    boot_files.mkdir()
    files = ('limine-bios.sys', 'KERNEL.ELF', 'INITAPP.ELF', 'limine.conf',
             'EFI/BOOT/BOOTX64.EFI')
    for name in files:
        target = boot_files/Path(name).name
        subprocess.run(['mcopy', '-i', f'{boot}@@2097152', f'::/{name}', str(target)], check=True)
    if args.kernel:
        # A diagnostic kernel belongs only to this private boot image; never
        # replace the acceptance template or alter the installed rootfs.
        shutil.copyfile(args.kernel.resolve(), boot_files/'KERNEL.ELF')
    subprocess.run(['mformat', '-i', f'{boot}@@2097152', '-T',
                    str(boot.stat().st_size//512 - 4096 - 33), '-F', '-v', 'LIMINEBOOT', '::'], check=True)
    for name in ('EFI', 'EFI/BOOT'):
        subprocess.run(['mmd', '-i', f'{boot}@@2097152', f'::/{name}'], check=True)
    for name in files:
        subprocess.run(['mcopy', '-i', f'{boot}@@2097152',
                        str(boot_files/Path(name).name), f'::/{name}'], check=True)
    subprocess.run(['mcopy', '-i', f'{boot}@@2097152', str(archive), '::/BOOTFS.IMG'], check=True)
    limine = Path('/nix/store/ak5j7i7ggyf3iqrgn7xmvdxb8cb3hp9l-limine-12.9.0/bin/limine')
    subprocess.run([str(limine), 'bios-install', str(boot), '1'], check=True)
    inputs = dict(backing=str(base), backing_sha256=digest(base),
                  boot_template_sha256=digest(boot_source/'limine-boot.img'),
                  kernel_sha256=digest(boot_files/'KERNEL.ELF'),
                  adapters=[dict(path=name, source=str(source), sha256=digest(source))
                            for name, source in installs],
                  bootfs=[dict(path=name, sha256=digest(source)) for name, source in entries],
                  scope='functional only; no LPR-inclusive performance measurement')
    (out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    prepare(SimpleNamespace(out=out, iso=iso, timeout=180, native_smoke=False), disk, installs)
    extra = []
    for name in ('usb', 'nvme'):
        scratch = out/f'{name}-scratch.img'
        subprocess.run(['qemu-img', 'create', '-q', '-f', 'raw', str(scratch), '16M'], check=True)
        extra += ['-drive', f'file={scratch},format=raw,if=none,id={name}probe,readonly=on']
    extra += ['-device', 'usb-storage,drive=usbprobe,bus=pachaxhci.0,id=usbprobe',
              '-device', 'nvme,drive=nvmeprobe,serial=fiber-nvme-probe']
    vm = None
    result = dict(status='FAIL', scope='desktop/network/USB; NVMe and exception gates are separate',
                  gpu=False, network=False, usb_hid=False,
                  usb_storage=False, nvme_sandbox=False, linux_warn_fixup=False)
    try:
        vm = VM(str(out/'run'), cpus=4, overlay=disk, disk=base, boot=boot,
                graphics='virgl', display='gtk,gl=on', input_profile='usb-hid',
                extra_qemu_args=extra, keep_overlay=True, boot_framebuffer=True, memory=args.memory)
        vm.wait_console(r'bash-\d+\.\d+#', args.timeout)
        vm.console_command('busybox timeout -s KILL 120 apk update && echo FIBER_NETWORK_PASS')
        vm.wait_console(r'^FIBER_NETWORK_PASS\r?$', 150)
        result['network'] = True
        vm.console_command('XFCE_APP_ONLY=terminal XFCE_APP_WINDOW_ATTEMPTS=120 '
                           'XFCE_DESKTOP_ATTEMPTS=120 bash /cmd/xfce_app_acceptance.sh --guest-controller &')
        vm.wait_console(r'XFCE_APP_ACCEPTANCE_DONE status=(?:PASS|FAIL)', args.timeout)
        console = (vm.out/'console.log').read_text(errors='replace')
        serial = (vm.out/'serial.log').read_text(errors='replace')
        combined = console+'\n'+serial
        if ('XFCE_APP_ACCEPTANCE_DONE status=PASS' not in console or
                not re.search(r'XFCE_GPU_XORG_INIT_PASS renderer=virgl \(D3D12 \(', console) or
                re.search(r'virgl \([^\n]*(?:LLVMPIPE|llvmpipe|softpipe)', combined)):
            raise RuntimeError('accelerated desktop gate failed')
        result['gpu'] = True
        # GL scanout need not expose a CPU DisplaySurface to QMP screendump.
        # Capture the actual GTK window, scoped by PID so another VM cannot
        # accidentally supply the desktop evidence. Keep ramfb attached.
        subprocess.run([sys.executable, str(ROOT/'tests/qemu_xfce_x11_capture.py')],
                       env=dict(os.environ, CAPTURE_QEMU_PID=str(vm.proc.pid),
                                CAPTURE_DIR=str(vm.out), CAPTURE_SECONDS='0'), check=True)
        result['desktop_capture'] = 'run/screen-0.0.png (host X11, matching QEMU PID)'
        if args.interactive:
            result['status'] = 'INCOMPLETE'
            result['pointer_stability'] = 'under investigation; user reported position jumps/flicker'
            print('INTERACTIVE_DESKTOP_READY; no automatic keyboard injection', flush=True)
            return
        # With no virtio keyboard present, a typed terminal command must have
        # traversed xHCI/HID/inputd, not just a virtconsole request.
        # startx stays alive after the acceptance marker. Keep it backgrounded
        # above, otherwise later console commands are merely echoed to startx,
        # not executed by the shell, and USB verification always times out.
        vm.console_command('echo FIBER_CONSOLE_READY; command -v xinput xdotool xprop')
        vm.wait_console(r'^FIBER_CONSOLE_READY\r?$', 15)
        keys = USBKeys(vm.qmp)
        keys.chord('ctrl', 'alt', 't')
        vm.console_command("DISPLAY=:0 busybox timeout 30 sh -c 'until xwininfo -root -tree | grep -q Terminal; do sleep .2; done; echo FIBER_INPUT_TERMINAL_READY'")
        vm.wait_console(r'^FIBER_INPUT_TERMINAL_READY\r?$', 40)
        type_text(keys, 'echo FIBER_USB_INPUT_PASS > /tmp/fiber-usb-input')
        keys.key('ret')
        vm.console_command("busybox timeout 30 sh -c 'until test -f /tmp/fiber-usb-input; do sleep .2; done; cat /tmp/fiber-usb-input'")
        vm.wait_console(r'^FIBER_USB_INPUT_PASS\r?$', 30)
        result['usb_hid'] = True
        serial = (vm.out/'serial.log').read_text(errors='replace')
        result['usb_storage'] = bool(re.search(r'\[usbd\] block sd\w+ .*gpt=-\d+', serial))
        result['nvme_sandbox'] = bool(re.search(r'\[usbd\] nvme ready', serial) and
                                      re.search(r'\[usbd\] block nvme\w+ .*gpt=-\d+', serial))
        if not result['usb_storage'] or vm.fault.is_set():
            raise RuntimeError('device runtime gates incomplete')
        if args.observe_seconds:
            print('POINTER_OBSERVATION_READY', flush=True)
            vm.console_command('DISPLAY=:0 xinput list; DISPLAY=:0 xdotool getmouselocation; echo FIBER_POINTER_INFO_DONE')
            vm.wait_console(r'^FIBER_POINTER_INFO_DONE\r?$', 30)
            deadline = time.monotonic() + args.observe_seconds
            while time.monotonic() < deadline:
                if vm.fault.is_set():
                    raise RuntimeError('guest fault during pointer observation')
                time.sleep(1)
            subprocess.run([sys.executable, str(ROOT/'tests/qemu_xfce_x11_capture.py')],
                           env=dict(os.environ, CAPTURE_QEMU_PID=str(vm.proc.pid),
                                    CAPTURE_DIR=str(vm.out), CAPTURE_SECONDS='1'), check=True)
        result['pointer_stability'] = 'unverified; user reported position jumps/flicker'
        result['status'] = 'PASS'
    except Exception as error:
        result['error'] = repr(error)
        raise
    finally:
        if args.keep_running and vm and vm.proc.poll() is None:
            # Interactive investigation must not lose the desktop on a failed
            # assertion. Offline fsck is deferred until this live VM is stopped.
            result['qemu_pid'] = vm.proc.pid
            serial = (vm.out/'serial.log').read_text(errors='replace')
            console = (vm.out/'console.log').read_text(errors='replace')
            renderer = re.search(r'XFCE_GPU_XORG_INIT_PASS renderer=([^\r\n]+)', console)
            result['gpu_initialization'] = renderer.group(1) if renderer else None
            result['usb_storage'] = bool(re.search(r'\[usbd\] block sd\w+ .*gpt=-\d+', serial))
            result['live'] = True
            result['fsck_deferred'] = 'VM remains live; offline checking is not safe yet'
            (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
            print(f'QEMU_LEFT_RUNNING pid={vm.proc.pid} qmp={vm.out}/qmp.sock', flush=True)
            try:
                while vm.proc.poll() is None:
                    time.sleep(1)
            finally:
                result['live'] = vm.proc.poll() is None
        if vm:
            vm.close()
            console = (vm.out/'console.log').read_text(errors='replace')
            serial = (vm.out/'serial.log').read_text(errors='replace')
            renderer = re.search(r'XFCE_GPU_XORG_INIT_PASS renderer=([^\r\n]+)', console)
            result['gpu_initialization'] = renderer.group(1) if renderer else None
            result['network'] = bool(re.search(r'^FIBER_NETWORK_PASS\r?$', console, re.M))
            result['usb_storage'] = bool(re.search(r'\[usbd\] block sd\w+ .*gpt=-\d+', serial))
        before = digest(disk)
        checker = out/'fsck.qcow2'
        overlay(checker, disk, 'qcow2')
        try:
            transcript = linux_script(SimpleNamespace(out=out, iso=iso, timeout=180),
                                      checker, 'fsck', 'fsck.sh', 'FILED_NVME_FSCK_OK')
            result['fsck_before_rc'] = int(re.search(r'NVME_FSCK_BEFORE_REPLAY_RC=(\d+)', transcript)[1])
            result['fsck_final_rc'] = int(re.search(r'NVME_FSCK_FINAL_RC=(\d+)', transcript)[1])
            if result['fsck_final_rc'] or digest(disk) != before:
                raise RuntimeError('offline filesystem check failed or changed the workload image')
        except Exception as error:
            result['status'] = 'FAIL'
            result['fsck_error'] = repr(error)
        result['backing_unchanged'] = digest(base) == inputs['backing_sha256']
        result['boot_template_unchanged'] = digest(boot_source/'limine-boot.img') == inputs['boot_template_sha256']
        if not result['backing_unchanged'] or not result['boot_template_unchanged']:
            result['status'] = 'FAIL'
            result['integrity_error'] = 'immutable backing or boot template changed'
        (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps(result), flush=True)
        if result['status'] != 'PASS' and 'error' not in result:
            raise RuntimeError('post-run integrity verification failed')


if __name__ == '__main__':
    main()
