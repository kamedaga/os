#!/usr/bin/env python3
"""Rootfs-free native VMO benchmark. Execution always requires KVM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time

from benchmark_filed_baseline import Guest

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / '.artifacts/vmo-memory-plan/stage2/bench'
CASES = {0: 'fd_get_info', 1: 'initial_fault', 2: 'discard_two_aliases',
         3: 'discard_with_edges', 4: 'refault', 5: 'unmapped_owned_discard_including_reclaim',
         6: 'unmapped_empty_discard_control'}
FAILURE = re.compile(r'NATIVE_VMO=FAIL|INVALID OPCODE|Kernel panic|PANIC|ACTION=terminate')


def parse(text, case):
    text = re.sub(r'\[Thread \d+\] ', '', text)
    if FAILURE.search(text):
        raise ValueError('guest failure')
    calibration = re.findall(r'NATIVE_VMO calibration_ticks=(\d+) calibration_ns=(\d+)', text)
    edges = re.findall(r'NATIVE_VMO views=(\d+) view_error_u64=(\d+) fd_edges=(\d+) ipc_edges=(\d+)', text)
    if len(calibration) != 1 or len(edges) != 1 or text.count('NATIVE_VMO=PASS') != 1:
        raise ValueError('missing/duplicate completion, calibration or edge count')
    ct, cn = map(int, calibration[0])
    if ct <= 0 or cn < 100_000_000:
        raise ValueError('bad calibration')
    rows = [dict(zip(('case', 'trial', 'ops', 'ticks'), map(int, row))) for row in re.findall(
        r'NATIVE_VMO case=(\d+) trial=(\d+) ops=(\d+) ticks=(\d+)', text)]
    if sorted(r['trial'] for r in rows) != list(range(1, 32)):
        raise ValueError('missing/duplicate trials')
    expected_ops = 65536 if case == 0 else 1024 if case in (1, 4) else 1
    for row in rows:
        if row['case'] != case or row['ops'] != expected_ops or row['ticks'] <= 0:
            raise ValueError('bad sample')
        row['ticks_per_op'] = row['ticks'] / row['ops']
        row['ns_per_op'] = row['ticks_per_op'] * cn / ct
    views, error, fds, ipc = map(int, edges[0])
    if case == 3 and (not 0 < views <= 10000 or fds != 64 or ipc != 19 or
                      (views < 10000 and error != 3) or (views == 10000 and error != 0)):
        raise ValueError('invalid stress setup; zero views is not a capacity result')
    if case != 3 and (views or error or fds or ipc):
        raise ValueError('unexpected edges')
    summary = {'case': case, 'name': CASES[case], 'trials': 31,
               'median_ticks_per_op': statistics.median(r['ticks_per_op'] for r in rows),
               'median_ns_per_op': statistics.median(r['ns_per_op'] for r in rows),
               'calibration_ticks': ct, 'calibration_ns': cn, 'views': views,
               'view_error_u64': error, 'fd_edges': fds, 'ipc_edges': ipc,
               'view_limit_reason': ('requested 10000 reached' if views == 10000 else
                   'page-view create returned ALLOC under 4G; see refusal diagnostics in serial.log'
                   if case == 3 else 'not applicable'),
               'view_refusal_diagnostics': [line for line in text.splitlines()
                                            if 'page-view' in line or 'page_view' in line],
               'ns_kind': 'derived from elapsed TSC / guest monotonic calibration',
               'ticks_kind': 'elapsed TSC ticks, not PMU cycles',
               'quarantine': 'not induced; no device ownership or DMA workload',
               'reclaim_isolated': False}
    return rows, summary


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def run(a):
    # Fail closed: never fall back to TCG or start QEMU for a parse-only check.
    with open('/dev/kvm', 'r+b', buffering=0):
        pass
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    kernel, init, bootfs, image = (p.resolve(strict=True) for p in
                                 (a.kernel, a.init, a.bootfs, a.boot_image))
    subprocess.run(['cp', '--reflink=auto', str(image), str(out / 'boot.img')], check=True)
    # Only a private small boot disk is copied. No root disk is attached.
    for source, target in ((kernel, 'KERNEL.ELF'), (init, 'INITAPP.ELF'), (bootfs, 'BOOTFS.IMG')):
        subprocess.run(['mcopy', '-o', '-i', str(out / 'boot.img') + '@@2097152',
                        str(source), '::/' + target], check=True)
        subprocess.run(['mcopy', '-i', str(out / 'boot.img') + '@@2097152',
                        '::/' + target, str(out / target)], check=True)
        if digest(source) != digest(out / target):
            raise ValueError('boot disk readback mismatch: ' + target)
    # Replace inherited boot options so neither old init nor a rootfs is selected.
    config = out / 'limine.conf'
    config.write_text('timeout: 0\nserial: yes\ngraphics: no\nverbose: yes\n'
                      '\n/Native VMO benchmark\n    protocol: limine\n'
                      '    path: boot():/KERNEL.ELF\n    resolution: 640x480x32\n')
    subprocess.run(['mcopy', '-o', '-i', str(out / 'boot.img') + '@@2097152',
                    str(config), '::/limine.conf'], check=True)
    command = [a.qemu, '-machine', 'q35', '-cpu', 'host', '-enable-kvm', '-m', '4G',
               '-smp', str(a.cpus), '-display', 'none', '-serial', 'stdio', '-monitor', 'none',
               '-no-reboot', '-net', 'none', '-device', 'intel-iommu,intremap=off,aw-bits=48',
               '-drive', f'file={out / "boot.img"},format=raw,if=ide']
    (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    (out / 'inputs.json').write_text(json.dumps({
        'kernel': str(kernel), 'kernel_sha256': digest(kernel), 'init': str(init),
        'init_sha256': digest(init), 'bootfs_sha256': digest(bootfs),
        'boot_image_sha256': digest(image), 'cpus': a.cpus, 'rootfs_attached': False,
        'host_affinity': sorted(os.sched_getaffinity(0)), 'physical_cpu_pinning': 'inherited',
        'source_hashes': {str(p.relative_to(ROOT)): digest(p) for p in (
            ROOT / 'tests/native_vmo_discard_bench.c', Path(__file__),
            ROOT / 'tests/build-native-vmo-discard-bench.sh', ROOT / 'tests/native_test_init.c')}
    }, indent=2) + '\n')
    guest = Guest(command, out / 'serial.log')
    try:
        deadline = time.monotonic() + a.timeout
        while b'NATIVE_VMO=PASS' not in guest.text:
            if guest.process.poll() is not None:
                raise RuntimeError('QEMU exited')
            if time.monotonic() >= deadline:
                raise TimeoutError('native benchmark timeout')
            for key, _ in guest.selector.select(1):
                chunk = os.read(key.fd, 65536)
                if chunk:
                    guest.log.write(chunk)
                    guest.log.flush()
                    guest.text.extend(chunk)
            if FAILURE.search(guest.text.decode(errors='replace')):
                raise RuntimeError('guest failure; see serial.log')
        rows, summary = parse(guest.text.decode(errors='replace'), a.case)
        (out / 'raw.json').write_text(json.dumps(rows, indent=2) + '\n')
        (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
    except Exception as error:
        (out / 'failure.json').write_text(json.dumps({'error': str(error)}) + '\n')
        raise
    finally:
        guest.close()


def aggregate(directory):
    result = {}
    for variant, cases in (('before', (0, 1)), ('after', range(7))):
        result[variant] = {}
        for case in cases:
            samples = [json.loads((directory / f'{variant}-{case}-{i}' / 'summary.json').read_text())
                       for i in range(1, 6)]
            if any(s['case'] != case for s in samples):
                raise ValueError('case mismatch')
            if case == 3 and len({s['views'] for s in samples}) != 1:
                raise ValueError('stress view counts differ across boots')
            result[variant][str(case)] = {key: statistics.median(s[key] for s in samples)
                for key in ('median_ticks_per_op', 'median_ns_per_op')}
            result[variant][str(case)]['boots'] = samples
    result['comparison'] = {}
    for unit in ('median_ticks_per_op', 'median_ns_per_op'):
        result['comparison'][unit] = {
            'fd_after_over_before': result['after']['0'][unit] / result['before']['0'][unit],
            'initial_fault_after_over_before': result['after']['1'][unit] / result['before']['1'][unit],
            'stress_over_plain_discard': result['after']['3'][unit] / result['after']['2'][unit],
            'refault_over_initial_fault': result['after']['4'][unit] / result['after']['1'][unit],
            'owned_minus_empty_unmapped_discard': result['after']['5'][unit] - result['after']['6'][unit]}
    result['limitations'] = [
        'owned-minus-empty includes backing validation, detach, metadata and reclaim; not isolated reclaim or ledger cost',
        'before source cannot be certified byte-identical for pre-existing untracked files; see snapshot-manifest.json',
        'stage2 whole-change comparison; fd delta does not isolate the hasDeferred load',
        'no induced quarantine; headless native benchmark has no graphics workload']
    (directory / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path)
    p.add_argument('--init', type=Path)
    p.add_argument('--bootfs', type=Path, default=ROOT / '.artifacts/BOOTFS.IMG')
    p.add_argument('--boot-image', type=Path, default=ROOT / '.artifacts/limine-boot.img')
    p.add_argument('--out', type=Path)
    p.add_argument('--case', type=int, choices=CASES)
    p.add_argument('--cpus', type=int, choices=(1, 2, 4), default=4)
    p.add_argument('--timeout', type=int, default=300)
    p.add_argument('--qemu', default=os.environ.get('CAPOS_QEMU', 'qemu-system-x86_64'))
    p.add_argument('--parse', type=Path, help='parse an existing serial log without QEMU')
    p.add_argument('--aggregate', type=Path)
    a = p.parse_args()
    if a.aggregate:
        aggregate(a.aggregate)
    elif a.parse:
        if a.case is None:
            p.error('--parse requires --case')
        print(json.dumps(parse(a.parse.read_text(), a.case)[1], indent=2))
    else:
        if a.kernel is None or a.init is None or a.case is None or a.out is None:
            p.error('execution requires --kernel --init --case --out')
        run(a)


if __name__ == '__main__':
    main()
