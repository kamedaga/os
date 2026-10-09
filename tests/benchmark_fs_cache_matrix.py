#!/usr/bin/env python3
"""Expanded IPC-free sandbox baseline with a matching dedicated Linux VM.

Cache eviction is guest-only. Existing disk images remain read-only backing
files, and incomplete workloads or failed offline fsck never become results.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess

from benchmark_filed_baseline import overlay
from validate_filed_nvme import digest, linux_script, save

ROOT = Path(__file__).resolve().parents[1]
PHASES = ('stat_d0', 'stat_d8', 'open_close_d0', 'open_close_d8',
          'create_close', 'rename', 'unlink', 'mkdir_rmdir', 'readdir_256',
          'readlink', 'pread_4k', 'pread_1m', 'pwrite_4k', 'pwrite_fsync')
ROW = re.compile(r'FS_CACHE_MATRIX fs=(\w+) phase=(\w+) '
                 r'cache=(\w+) trial=(\d+) ops=(\d+) ns=(\d+)\r?\n')


def config_values(text):
    values = dict(re.findall(r'^(CONFIG_\w+)=(.*)$', text, re.M))
    values.update({key: 'n' for key in re.findall(r'^# (CONFIG_\w+) is not set$', text, re.M)})
    return values


def check_storage_config(actual, profile, allow_historical=False):
    have, expected = config_values(actual), config_values(profile)
    differences = {key: {'actual': have.get(key, 'n'), 'expected': value}
                   for key, value in expected.items() if have.get(key, 'n') != value}
    if differences and not allow_historical:
        raise RuntimeError(f'Linux config differs from current storage profile: {differences}; '
                           'rebuild, or explicitly label a historical comparison with --allow-historical-config')
    return differences
STAGES = re.compile(r'FS_CACHE_STAGES fs=(\w+) phase=(\w+) cache=(\w+) '
                    r'trial=(\d+) ops=(\d+) calls=(\d+) encode=(\d+) '
                    r'submit=(\d+) wait=(\d+) inspect=(\d+) recycle=(\d+)\r?\n')
STAGE_NAMES = ('encode', 'submit', 'wait', 'inspect', 'recycle')
RECYCLE = re.compile(r'FS_RECYCLE_CONTROL fs=(\w+) phase=(\w+) cache=(\w+) '
                     r'trial=(\d+) scrub=([01]) ops=(\d+) ns=(\d+) max_ns=(\d+) '
                     r'tail_ops=(\d+) tail_ns=(\d+) io_probed=([01]) reads=(\d+) writes=(\d+) switches=(\d+) '
                     r'tail_reads=(\d+) tail_writes=(\d+) tail_switches=(\d+)\r?\n')


def collect_recycle(text, rows):
    expected = {(r['fs'], r['phase'], r['cache'], r['trial']): r for r in rows}
    result = {}
    for fs, phase, cache, trial, scrub, ops, ns, maximum, *counters in RECYCLE.findall(text):
        cell = fs, phase, cache, int(trial)
        key = (*cell, int(scrub))
        ops, ns, maximum = int(ops), int(ns), int(maximum)
        if cell not in expected or key in result:
            raise RuntimeError(f'unexpected/duplicate recycle record: {key}')
        if ops * 2 != expected[cell]['ops'] or not 0 < maximum <= ns or ns > maximum * ops:
            raise RuntimeError(f'invalid recycle count/time: {key}')
        names = ('tail_ops', 'tail_ns', 'io_probed', 'reads', 'writes', 'switches',
                 'tail_reads', 'tail_writes', 'tail_switches')
        counts = dict(zip(names, map(int, counters)))
        tails, tail_ns = counts['tail_ops'], counts['tail_ns']
        probe = fs == 'ext4' and cache == 'warm' and phase in ('create_close', 'rename', 'unlink', 'mkdir_rmdir', 'pwrite_fsync')
        if (counts['io_probed'] != probe or not 0 <= tails <= ops or
                not tails * 100000 <= tail_ns <= min(ns, tails * maximum) or
                (maximum >= 100000) != bool(tails) or
                (tails == ops and tail_ns != ns) or
                (tails < ops and ns - tail_ns >= (ops - tails) * 100000) or
                any(counts['tail_' + k] > counts[k] for k in ('reads', 'writes', 'switches')) or
                (not tails and any(counts['tail_' + k] for k in ('reads', 'writes', 'switches'))) or
                (not probe and any(counts[k] for k in names[3:]))):
            raise RuntimeError(f'invalid recycle tail/counters: {key}')
        result[key] = dict(fs=fs, phase=phase, cache=cache, trial=int(trial),
                           scrub=bool(int(scrub)), ops=ops, ns=ns,
                           ns_per_op=ns / ops, max_ns=maximum, **counts)
    if result.keys() != {(*cell, scrub) for cell in expected for scrub in (0, 1)}:
        raise RuntimeError('missing recycle records')
    for cell, row in expected.items():
        if sum(result[(*cell, scrub)]['ns'] for scrub in (0, 1)) != row['ns']:
            raise RuntimeError(f'recycle times do not sum to outer measurement: {cell}')
    return {'rows': list(result.values()),
            'caveat': 'diagnostic same-binary ABBA payload scrub; 32-operation warm blocks, single-operation cold/fsync blocks; tails >=100us remain in full totals; read-only I/O/switch counters outside timing for ext4 warm mutations/fsync; both policies retain metadata zero, EOF tails and release/task_work; mixed cache history, not independent normal baselines'}


def collect_stages(text, rows):
    clocks = re.findall(r'FS_CACHE_STAGE_CLOCK ticks=(\d+) ns=(\d+)\r?\n', text)
    if len(clocks) != 1 or any(int(v) <= 0 for v in clocks[0]):
        raise RuntimeError('missing/duplicate/invalid stage clock calibration')
    ticks, ns = map(int, clocks[0])
    scale = ns / ticks
    expected = {(r['fs'], r['phase'], r['cache'], r['trial']): r for r in rows}
    result = {}
    multipliers = {'open_close_d0': 2, 'open_close_d8': 2, 'create_close': 2,
                   'mkdir_rmdir': 2, 'readdir_256': 3, 'pwrite_fsync': 2}
    for fs, phase, cache, trial, ops, calls, *values in STAGES.findall(text):
        key = fs, phase, cache, int(trial)
        if key not in expected or key in result:
            raise RuntimeError(f'unexpected/duplicate stage record: {key}')
        row = expected[key]
        if int(ops) != row['ops'] or int(calls) != row['ops'] * multipliers.get(phase, 1) or any(int(v) <= 0 for v in values):
            raise RuntimeError(f'incomplete stage operations/calls/time: {key}')
        stage_ns = {name: int(value) * scale / int(ops) for name, value in zip(STAGE_NAMES, values)}
        result[key] = {**row, 'calls': int(calls), 'stages_ns_per_op': stage_ns,
                       'outside_stamps_ns_per_op': row['ns_per_op'] - sum(stage_ns.values())}
    if result.keys() != expected.keys():
        raise RuntimeError('missing stage records')
    return {'clock': {'ticks': ticks, 'ns': ns}, 'rows': list(result.values()),
            'caveat': 'elapsed TSC ticks converted using whole-run calibration; wrapper probes included; submit may contain inline VFS OR worker wake/handoff/execution before returning, so submit+wait is the combined admission-to-result interval; not additive to nested service profiles or CPU time'}


def verify_checkpoint(text, expected):
    if text.count('FS_CACHE_POLICY cold=checkpoint_then_drop drop_passes=4') != 1:
        raise RuntimeError('missing or inconsistent checkpoint-before-drop policy')
    counts = re.findall(r'FS_CACHE_CHECKPOINT prepared=(\d+)\r?\n', text)
    if counts != [str(expected)]:
        raise RuntimeError(f'incomplete checkpoint preparation: {counts}, expected {expected}')
    return expected


def collect(text):
    expected = {(fs, phase, cache, trial) for fs in ('ext4', 'tmpfs')
                for phase in PHASES for cache in (('warm', 'cold') if fs == 'ext4' else ('warm',))
                for trial in range(1, 4)}
    rows = {}
    for fs, phase, cache, trial, ops, ns in ROW.findall(text):
        key = fs, phase, cache, int(trial)
        if key not in expected or key in rows:
            raise RuntimeError(f'unexpected or duplicate record: {key}')
        ops, ns = int(ops), int(ns)
        count = 12 if cache == 'cold' else 24 if phase == 'pwrite_fsync' else 1024
        if ops != count or ns <= 0:
            raise RuntimeError(f'invalid sample count/time: {key}')
        rows[key] = {'fs': fs, 'phase': phase, 'cache': cache,
                     'trial': int(trial), 'ops': ops, 'ns': ns, 'ns_per_op': ns / ops}
    if set(rows) != expected:
        raise RuntimeError(f'missing records: {expected - set(rows)}')
    return list(rows.values())


def fsck(args, disk, role):
    before = digest(disk)
    checker = args.out / (role + '-fsck.qcow2')
    overlay(checker, disk, 'qcow2')
    text = linux_script(args, checker, role + '-fsck', 'fsck.sh', 'FILED_NVME_FSCK_OK')
    if 'NVME_FSCK_FINAL_RC=0' not in text or digest(disk) != before:
        raise RuntimeError(f'{role}: offline fsck failed or source disk changed')
    result = {'before_rc': int(re.search(r'NVME_FSCK_BEFORE_REPLAY_RC=(\d+)', text)[1]),
              'final_rc': 0, 'source_unchanged': True}
    save(args.out / (role + '-fsck.json'), result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--adapter', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--base', type=Path, default=ROOT / '.artifacts/fs-sandbox-a-clock-page/base1')
    parser.add_argument('--inventory', type=Path,
                        default=ROOT / '.artifacts/kobox2-storage-production/closure-inventory.json')
    parser.add_argument('--iso', type=Path,
                        default=ROOT / '.artifacts/fs-bench/baseline-20261002/alpine-virt-3.22.5-x86_64.iso')
    parser.add_argument('--timeout', type=int, default=300)
    diagnostics = parser.add_mutually_exclusive_group()
    diagnostics.add_argument('--diagnostic', action='store_true')
    diagnostics.add_argument('--stages', action='store_true')
    diagnostics.add_argument('--recycle-control', action='store_true')
    parser.add_argument('--tail-trace', action='store_true')
    parser.add_argument('--linux-config', type=Path, required=True,
                        help='Actual canonical .config, hash-checked against the linked core')
    parser.add_argument('--allow-historical-config', action='store_true')
    args = parser.parse_args()
    for name in ('out', 'runtime', 'adapter', 'native', 'base', 'inventory', 'iso'):
        setattr(args, name, getattr(args, name).resolve())
    flags = json.loads((args.runtime / 'linux-boot-inputs.json').read_text())['extra_cflags']
    linux_identity = json.loads((args.runtime / 'linux-boot-inputs.json').read_text())['linux']
    if digest(args.linux_config) != linux_identity['.config']:
        raise RuntimeError('supplied Linux config does not match linked core')
    config_differences = check_storage_config(args.linux_config.read_text(),
        (ROOT / 'kobox2/linux-sandbox/kobox/manifest/profiles/storage.config').read_text(),
        args.allow_historical_config)
    if '-DKOBOX_FS_CACHE_MATRIX=1' not in flags or '-DKOBOX_FS_BENCH_ELAPSED_ONLY=1' not in flags:
        raise RuntimeError('not an elapsed-only cache-matrix core')
    if ('-DKOBOX_FS_CACHE_DIAGNOSTIC=1' in flags) != args.diagnostic:
        raise RuntimeError('diagnostic flag must match runtime; do not publish a diagnostic baseline')
    if ('-DKOBOX_FS_CACHE_STAGES=1' in flags) != args.stages:
        raise RuntimeError('stage flag must match runtime; do not publish a diagnostic baseline')
    if ('-DKOBOX_FS_RECYCLE_CONTROL=1' in flags) != args.recycle_control:
        raise RuntimeError('recycle flag must match runtime; do not publish a diagnostic baseline')
    if ('-DKOBOX_FS_TAIL_TRACE=1' in flags) != args.tail_trace or (args.tail_trace and not args.recycle_control):
        raise RuntimeError('tail trace requires matching runtime and recycle control')
    if any('KOBOX_FS_SERVICE_PROFILE' in flag or 'KOBOX_FS_INLINE_DISABLED' in flag for flag in flags):
        raise RuntimeError('unexpected instrumentation or non-production inline policy')
    inventory = json.loads(args.inventory.read_text())
    if any(not (args.runtime / 'modules' / path).is_file() for path in inventory['load_order']):
        raise RuntimeError('module build is incomplete')
    args.out.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(args.linux_config, args.out / 'linux.config')
    guest_dir = args.out / 'guest'
    guest_dir.mkdir()
    shutil.copyfile(args.native, guest_dir / 'native.elf')
    if args.diagnostic:
        (guest_dir / 'cold-diagnostic').touch()
    for source in (ROOT / '.artifacts/alpine-installer-apk').glob('*.apk'):
        shutil.copyfile(source, guest_dir / source.name)
    for source, target in (('filed_nvme_fsck.sh', 'fsck.sh'),
                           ('fs_cache_matrix_linux.sh', 'native.sh')):
        shutil.copyfile(ROOT / 'tests/fixtures' / source, guest_dir / target)
    package = args.out / 'package'
    subprocess.run(['python3', str(ROOT / 'kobox2/linux-sandbox/kobox/manifest/build_pci_package.py'),
                    '--inventory', str(args.inventory), '--runtime-dir', str(args.runtime),
                    '--output-dir', str(package), '--protocol-dir', str(ROOT / 'kobox2/protocol'),
                    '--profile', 'storage', '--generator-dir', str(args.out / 'manifest-build')], check=True)
    shutil.copyfile(args.adapter, package / 'sandbox.elf')
    base = json.loads((args.base / 'inputs.json').read_text())
    if not all(json.loads((args.base / 'gates.json').read_text()).values()):
        raise RuntimeError('base verification failed')
    selected = {row['path']: row['source'] for row in base['bootfs_inputs']}
    backing = args.base / 'prepared.qcow2'
    backing_hash = digest(backing)
    sources = [ROOT / 'tests/fs_cache_matrix_linux.c', Path(__file__),
               ROOT / 'tests/fixtures/fs_cache_matrix_linux.sh'] + [
        ROOT / 'kobox2/linux-sandbox/kobox/boot' / name for name in
        ('fs_cache_matrix.h', 'fs_cache_matrix.inc', 'fs_bench.c', 'fs_bench.h',
         'fs_port.c', 'fs_port_internal.h', 'fs_handle_cache.h', 'fs_service.c',
         'fs_worker.c', 'build_boot_runtime.py')]
    sources += [ROOT / 'kobox2/linux-sandbox/kobox/task/port.c',
                ROOT / 'kobox2/linux-sandbox/kobox/task/diagnostic.h']
    sources += list((ROOT / 'userland/kobox2_adapter').glob('tail_trace.*'))
    save(args.out / 'inputs.json', {
        'storage_profile_differences': config_differences,
        'flags': flags, 'backing_sha256': backing_hash,
        'runtime_sha256': digest(args.runtime / 'linux-boot-runtime.so'),
        'adapter_sha256': digest(args.adapter), 'native_sha256': digest(args.native),
        'kernel_sha256': digest(base['kernel']['source']),
        'linux_build': json.loads((args.runtime / 'linux-boot-inputs.json').read_text())['linux'],
        'sources': {str(path.relative_to(ROOT)): digest(path) for path in sources},
        'boundary': 'single-flight; per-operation wall time; sandbox encode/submit/collect/release; Linux userspace syscall; no LPR/IPC',
        'cold': 'guest sync_filesystem/syncfs, full freeze/thaw checkpoint drain, then 4 clean-page + reclaimable-slab eviction passes; outside timing; pinned objects and host/device caches are not evicted',
        'tmpfs': 'warm only; tmpfs data is not discardable disk-backed pagecache'})
    command = ['python3', str(ROOT / 'tests/qemu_filed_cutover_smoke.py'),
               '--out', str(args.out / 'sandbox'), '--iso', str(args.iso),
               '--backing', str(backing), '--kernel', base['kernel']['source'],
               '--init-app', base['init_app']['source'],
               '--boot-image', str(args.base / 'limine-boot.img'),
               '--storage-boot', selected['/srv/storage_boot.elf'],
               '--seed0root', selected['/sbin/seed0root.elf'], '--filed', selected['/sbin/filed.elf'],
               '--package', str(package), '--sandbox-bench-only', '--timeout', str(args.timeout)]
    if args.diagnostic:
        events = args.out / 'nvme-events'
        events.write_text('pci_nvme_read\npci_nvme_write\npci_nvme_rw_complete_cb\n')
        command += ['--nvme-trace', str(events)]
    save(args.out / 'sandbox-command.json', command)
    print('CACHE_MATRIX_SANDBOX_BEGIN', flush=True)
    try:
        subprocess.run(command, check=True)
        sandbox_text = (args.out / 'sandbox/pacha.log').read_text()
        if re.search(r'WARNING: CPU:|BUG:|Oops:|FS_CACHE_MATRIX_FAIL', sandbox_text):
            raise RuntimeError('sandbox warning/fault; this run cannot become a baseline')
        sandbox = collect(sandbox_text)
        stages = collect_stages(sandbox_text, sandbox) if args.stages else None
        recycle = collect_recycle(sandbox_text, sandbox) if args.recycle_control else None
        if args.tail_trace:
            status = re.findall(r'PH_TAIL_STATUS count=0x([0-9a-f]+) dropped=0x([0-9a-f]+)', sandbox_text)
            core_status = re.findall(r'FS_TAIL_REQUEST_DONE count=(\d+) dropped=(\d+)', sandbox_text)
            native_rows = re.findall(r'PH_TAIL kind=0x([0-9a-f]+) begin=0x([0-9a-f]+) end=0x([0-9a-f]+) object=0x([0-9a-f]+) cpu=0x([0-9a-f]+)', sandbox_text)
            requests = re.findall(r'FS_TAIL_REQUEST phase=(\w+) trial=(\d+) sample=(\d+) scrub=(\d+) begin=(\d+) end=(\d+) ns=(\d+) cpu=(\d+) end_cpu=(\d+)', sandbox_text)
            task_rows = re.findall(r'FS_TAIL_TASK kind=(\d+) cpu=(\d+) end_cpu=(\d+) notification=(\d+) pid=(\d+) next=(\d+) comm=(\S+) t0=(\d+) t1=(\d+) t2=(\d+) t3=(\d+) t4=(\d+) work=(\d+) last_work=(\d+)', sandbox_text)
            task_status = re.findall(r'FS_TAIL_TASK_DONE count=(\d+) ready=(\d+)', sandbox_text)
            if (len(status) != 9 or any(int(d, 16) for n, d in status) or
                    len(core_status) != 1 or core_status[0] != (str(len(requests)), '0') or
                    int(status[-1][0], 16) != len(native_rows) or
                    task_status != [(str(len(task_rows)), str(len(task_rows)))]):
                raise RuntimeError('incomplete/overflowed native tail trace')
            expected_tails = sum(r['tail_ops'] for r in recycle['rows']
                                 if r['fs'] == 'ext4' and r['cache'] == 'warm' and r['phase'] in ('create_close', 'rename', 'unlink'))
            if len(requests) != expected_tails:
                raise RuntimeError('tail request trace does not match full sample counters')
            tail_trace = {'native': [dict(zip(('kind', 'begin', 'end', 'object', 'cpu'), (int(v, 16) for v in row))) for row in native_rows],
                          'requests': [dict(phase=row[0], **dict(zip(('trial', 'sample', 'scrub', 'begin', 'end', 'ns', 'cpu', 'end_cpu'), map(int, row[1:])))) for row in requests],
                          'tasks': [dict(comm=row[6], **dict(zip(('kind', 'cpu', 'end_cpu', 'notification', 'pid', 'next', 't0', 't1', 't2', 't3', 't4', 'work', 'last_work'), map(int, row[:6] + row[7:])))) for row in task_rows],
                          'anchor': int(re.findall(r'FS_TAIL_ANCHOR address=(\d+)', sandbox_text)[0])}
        sandbox_checkpoints = verify_checkpoint(sandbox_text, 1512 if args.diagnostic else 504)
        if not all(json.loads((args.out / 'sandbox/gates.json').read_text()).values()):
            raise RuntimeError('sandbox gates failed')
    finally:
        if (args.out / 'sandbox/pacha.qcow2').exists():
            fsck(args, args.out / 'sandbox/pacha.qcow2', 'sandbox')
    disk = args.out / 'native.qcow2'
    overlay(disk, backing, 'qcow2')
    print('CACHE_MATRIX_LINUX_BEGIN', flush=True)
    try:
        native_text = linux_script(args, disk, 'native', 'native.sh', 'FS_CACHE_NATIVE_OK')
        if not re.search(r'FS_CACHE_MATRIX_DONE status=0\r?\n', native_text):
            raise RuntimeError('Linux benchmark failed')
        if re.search(r'WARNING: CPU:|BUG:|Oops:', native_text):
            raise RuntimeError('Linux warning/fault; reference run rejected')
        native = collect(native_text)
        native_checkpoints = verify_checkpoint(native_text, 504)
    finally:
        fsck(args, disk, 'native')
    if digest(backing) != backing_hash:
        raise RuntimeError('backing changed')
    result = {'accepted': not (args.diagnostic or args.stages or args.recycle_control),
              'diagnostic': args.diagnostic or args.stages or args.recycle_control,
              'unit': 'microseconds/op', 'trials': 3, 'backing_unchanged': True,
              'current_storage_profile': not config_differences,
              'storage_profile_differences': config_differences,
              'cold_policy': 'checkpoint_then_drop',
              'checkpoint_preparations': {'sandbox': sandbox_checkpoints, 'linux': native_checkpoints},
              'raw': {'sandbox': sandbox, 'linux': native}, 'medians': {}}
    if args.stages:
        result['stage_diagnostic'] = stages
    if args.recycle_control:
        result['recycle_diagnostic'] = recycle
    if args.tail_trace:
        result['tail_trace'] = tail_trace
    for fs in ('ext4', 'tmpfs'):
        result['medians'][fs] = {}
        for phase in PHASES:
            values = {}
            for role, records in (('sandbox', sandbox), ('linux', native)):
                for cache in (('warm', 'cold') if fs == 'ext4' else ('warm',)):
                    values[role + '_' + cache] = statistics.median(
                        row['ns_per_op'] / 1000 for row in records
                        if (row['fs'], row['phase'], row['cache']) == (fs, phase, cache))
            result['medians'][fs][phase] = values
    save(args.out / 'summary.json', result)
    print('FS_CACHE_MATRIX_OK', args.out, flush=True)


if __name__ == '__main__':
    main()
