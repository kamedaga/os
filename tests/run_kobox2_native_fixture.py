#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run a native init fixture on a private boot-only image; no rootfs or build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def gates_in(text, gates):
    # A diagnostic quoting an expected marker must not satisfy a runtime gate.
    return {gate: bool(re.search(r'(?m)^(?:\[Thread \d+\] )?' + re.escape(gate) + r'\r?$', text))
            for gate in gates}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--init', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--boot-template', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gate', action='append', required=True)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    producers = [p.resolve(strict=True) for p in (args.init, args.kernel, args.boot_template)]
    if not 2 * 1024 * 1024 < producers[2].stat().st_size <= 128 * 1024 * 1024:
        parser.error('require a small raw bootloader template, not a rootfs disk')
    if args.manifest:
        args.manifest = args.manifest.resolve(strict=True)
        producers.append(args.manifest)
    before = {str(p): digest(p) for p in producers}
    boot = args.out / 'boot.img'
    # Only the small bootloader template is reflinked; never attach a rootfs.
    subprocess.run(['cp', '--reflink=auto', str(producers[2]), str(boot)], check=True)
    for source, destination in ((producers[1], 'KERNEL.ELF'), (producers[0], 'INITAPP.ELF')):
        subprocess.run(['mcopy', '-o', '-i', str(boot) + '@@2097152', str(source), '::/' + destination], check=True)
    if args.manifest:
        bootfs = args.out / 'bootfs.img'
        subprocess.run(['go', 'run', './cmd/bootfs-image', '--manifest', str(args.manifest),
                        '--output', str(bootfs)], cwd=ROOT / 'pack', check=True)
        subprocess.run(['mcopy', '-o', '-i', str(boot) + '@@2097152', str(bootfs), '::/BOOTFS.IMG'], check=True)
    serial = args.out / 'serial.log'
    command = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host',
               '-m', '2G', '-smp', '2', '-device', 'intel-iommu,intremap=off,aw-bits=48',
               '-drive', f'file={boot},format=raw,if=ide', '-display', 'none',
               '-serial', f'file:{serial}', '-monitor', 'none', '-no-reboot']
    evidence = {'producers': before, 'command': command, 'required_gates': args.gate}
    if args.manifest:
        evidence['bootfs_sha256'] = digest(bootfs)
    (args.out / 'inputs.json').write_text(json.dumps(evidence, indent=2) + '\n')
    text = ''
    with (args.out / 'qemu.log').open('w') as output:
        child = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                text = serial.read_text(errors='replace') if serial.exists() else ''
                if all(gates_in(text, args.gate).values()) or re.search(
                        r'=FAIL|ACTION=terminate|Kernel panic|PANIC|init ELF load failed', text):
                    break
                if child.poll() is not None:
                    break
                time.sleep(.1)
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    text = serial.read_text(errors='replace') if serial.exists() else ''
    unchanged = all(digest(Path(p)) == value for p, value in before.items())
    gates = gates_in(text, args.gate)
    failure = bool(re.search(r'=FAIL|ACTION=terminate|Kernel panic|PANIC|init ELF load failed', text))
    result = {'gates': gates, 'inputs_unchanged': unchanged, 'unexpected_failure': failure,
              'ok': all(gates.values()) and unchanged and not failure}
    (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if not result['ok']:
        print(text[-8000:])
        raise SystemExit(1)
    print('KOBOX2_NATIVE_FIXTURE_OK', args.out)

if __name__ == '__main__':
    main()
