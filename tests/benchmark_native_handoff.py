#!/usr/bin/env python3
"""Run the empty native thread pair on a private, rootfs-free boot image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

from benchmark_filed_baseline import Guest

ROOT = Path(__file__).resolve().parents[1]


def parse(text):
    # Native log syscalls prefix each fragment, including separately printed numbers.
    text = re.sub(r'\[Thread \d+\] ', '', text)
    if re.search(r'NATIVE_HANDOFF=FAIL|INVALID OPCODE|Kernel panic|PANIC|ACTION=terminate', text):
        raise ValueError('guest failure')
    rows = [dict(zip(('trial', 'rounds', 'ticks', 'elapsed_ns'), map(int, match)))
            for match in re.findall(
                r'NATIVE_HANDOFF trial=(\d+) rounds=(\d+) ticks=(\d+) elapsed_ns=(\d+)', text)]
    if sorted(row['trial'] for row in rows) != [1, 2, 3] or 'NATIVE_HANDOFF=PASS' not in text:
        raise ValueError('missing or duplicate trial/completion')
    for row in rows:
        if row['rounds'] < 128 or row['rounds'] % 128 or row['ticks'] <= 0 or row['elapsed_ns'] < 10**9:
            raise ValueError('invalid trial')
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--init', type=Path, required=True)
    p.add_argument('--bootfs', type=Path, required=True)
    p.add_argument('--boot-image', type=Path, default=ROOT / '.artifacts/limine-boot.img')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--cpus', type=int, choices=(2, 4), default=4)
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    kernel, init, bootfs = (v.resolve(strict=True) for v in (a.kernel, a.init, a.bootfs))
    shutil.copyfile(init, out / 'init.elf')
    shutil.copyfile(bootfs, out / 'bootfs.img')
    # This is the small boot disk, not a rootfs copy; no filesystem workload is attached.
    subprocess.run(['cp', '--reflink=auto', str(a.boot_image), str(out / 'boot.img')], check=True)
    for source, target in ((kernel, 'KERNEL.ELF'), (out / 'init.elf', 'INITAPP.ELF'),
                           (out / 'bootfs.img', 'BOOTFS.IMG')):
        subprocess.run(['mcopy', '-o', '-i', str(out / 'boot.img') + '@@2097152',
                        str(source), '::/' + target], check=True)

    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    command = ['qemu-system-x86_64', '-machine', 'q35', '-cpu', 'host', '-enable-kvm',
               '-m', '4G', '-smp', str(a.cpus), '-display', 'none', '-serial', 'stdio',
               '-monitor', 'none', '-no-reboot', '-net', 'none',
               '-device', 'intel-iommu,intremap=off,aw-bits=48',
               '-drive', f'file={out / "boot.img"},format=raw,if=ide']
    (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    (out / 'inputs.json').write_text(json.dumps({
        'rootfs_attached': False, 'cpus': a.cpus, 'kernel_sha256': digest(kernel),
        'init_sha256': digest(out / 'init.elf'), 'bootfs_sha256': digest(out / 'bootfs.img'),
        'sources': {str(path.relative_to(ROOT)): digest(path) for path in (
            ROOT / 'tests/native_handoff_bench.c', ROOT / 'tests/native_test_init.c',
            ROOT / 'tests/build-native-handoff-bench.sh', Path(__file__).resolve(),
            ROOT / 'userland/libpacha/src/syscall.c', ROOT / 'userland/libpacha/include/pacha/abi.h')}
    }, indent=2) + '\n')
    guest = Guest(command, out / 'serial.log')
    try:
        deadline = time.monotonic() + 45
        while b'NATIVE_HANDOFF=PASS' not in guest.text:
            assert guest.process.poll() is None, 'QEMU exit'
            assert time.monotonic() < deadline, 'native timeout'
            for key, _ in guest.selector.select(1):
                chunk = os.read(key.fd, 65536)
                if chunk:
                    guest.log.write(chunk)
                    guest.log.flush()
                    guest.text.extend(chunk)
            assert not re.search(rb'NATIVE_HANDOFF=FAIL[^\r\n]*\r?\n|INVALID OPCODE|Kernel panic|PANIC|ACTION=terminate',
                                 guest.text), 'guest failure; see serial.log'
        rows = parse(guest.text.decode(errors='replace'))
        (out / 'raw.json').write_text(json.dumps(rows, indent=2) + '\n')
        summary = {
            'elapsed_ns_per_roundtrip': statistics.median(r['elapsed_ns'] / r['rounds'] for r in rows),
            'ticks_per_roundtrip': statistics.median(r['ticks'] / r['rounds'] for r in rows),
            'boundary': 'two native threads; permit plus compound wake/wait; no Linux/FS',
            'physical_cpu_affinity': 'not pinned; migration allowed',
            'ticks_kind': 'elapsed TSC ticks, not PMU executed cycles',
        }
        (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
    except Exception as error:
        (out / 'failure.json').write_text(json.dumps({'error': str(error)}, indent=2) + '\n')
        raise
    finally:
        guest.close()


if __name__ == '__main__':
    main()
