#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate a verified normal filed cutover on private NVMe COW overlays."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import time
from types import SimpleNamespace

from benchmark_filed_baseline import overlay, qemu_common, parse, PHASES, linux
from qemu_filed_cutover_smoke import CutoverGuest, prepare

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def line_marker(name):
    return re.compile(rb'(?:^|\n)' + re.escape(name.encode()) + rb'\r?\n')


def linux_script(args, disk, role, script, marker):
    command = qemu_common(disk) + ['-cdrom', str(args.iso), '-boot', 'order=d',
        '-drive', f'file=fat:ro:{args.out / "guest"},format=raw,if=virtio,readonly=on']
    save(args.out / (role + '-command.json'), command)
    guest = CutoverGuest(command, args.out / (role + '.log'))
    try:
        guest.wait('localhost login:', args.timeout)
        guest.send('root')
        guest.wait('localhost:~#', args.timeout)
        guest.send('(mkdir -p /artifacts && mount -o ro /dev/vda1 /artifacts && '
                   f'sh /artifacts/{script}) || echo FS_BASELINE_\'ERROR\'')
        guest.wait(line_marker(marker), args.timeout)
        guest.send('poweroff')
        guest.process.wait(timeout=30)
        return guest.text.decode(errors='replace')
    finally:
        guest.close()


def pacha_script(args, disk, script, marker, parameters=''):
    console = args.out / 'console.sock'
    boot = args.out / 'boot.qcow2'
    overlay(boot, args.boot_image, 'raw')
    command = qemu_common(disk) + ['-drive', f'file={boot},format=qcow2,if=ide',
        '-boot', 'order=c', '-chardev', f'socket,id=virtcon0,path={console},server=on,wait=off',
        '-device', 'virtio-serial-pci,disable-legacy=on,iommu_platform=on,id=virtserial0',
        '-device', 'virtconsole,chardev=virtcon0,name=org.pachaos.console.0']
    command[command.index('-m') + 1] = f'{args.memory_mib}M'
    save(args.out / 'pacha-command.json', command)
    guest = CutoverGuest(command, args.out / 'pacha.log')
    try:
        deadline = time.monotonic() + 15
        while not console.exists():
            if guest.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('console socket did not appear')
            time.sleep(.05)
        guest.connect(console)
        guest.wait('[termd] linux tty hvc open ready index=0 handle=', 300)
        guest.wait(re.compile(rb'(?:^|\n)[^\r\n]*bash-\d+\.\d+# (?=\r?\n|$)'),
                   300, console_only=True)
        guest.send(f'(bash {shlex.quote(script)} {parameters} && '
                   "printf 'filed-nvme-synced\\n' > /root/filed-nvme-persistence && sync && "
                   f'echo {marker}) || echo FS_BASELINE_\'ERROR\'')
        guest.wait(line_marker(marker), args.timeout, console_only=True)
        return guest.console_text.decode(errors='replace')
    finally:
        guest.close()
        (args.out / 'console.log').write_bytes(getattr(guest, 'console_text', b''))


def validate_clocks(texts, trials):
    clocks = {key: [int(v) for v in re.findall(r'tsc_hz=(\d+)', text)]
              for key, text in texts.items()}
    if any(len(v) != 2 * trials for v in clocks.values()):
        raise RuntimeError('missing TSC calibration')
    hz = [v for values in clocks.values() for v in values]
    if min(hz) <= 0 or max(hz) / min(hz) > 1.01:
        raise RuntimeError('TSC calibration mismatch')
    return clocks


def summarize(current, native, old, clocks):
    result = {}
    for fs in ('ext4', 'tmpfs'):
        result[fs] = {}
        for phase in sorted(PHASES):
            now = statistics.median(r['ns_per_op'] for r in current[fs][phase])
            target = statistics.median(r['ns_per_op'] for r in native[fs][phase])
            previous = old[fs][phase]['pacha_median_ns']
            result[fs][phase] = {'old_pacha_median_ns': previous,
                'new_pacha_median_ns': now, 'linux_median_ns': target,
                'old_over_new': previous / now, 'new_over_linux': now / target}
            # The identical baseline ELF reports calibrated elapsed TSC time,
            # not CPU-unhalted cycles. Recover ticks with each run's calibration
            # instead of using nominal CPU frequency or a different workload.
            for role, data in (('pacha', current), ('linux', native)):
                cycles = [row['total_ns'] / row['ops'] *
                          clocks[role][2 * index + (fs == 'tmpfs')] / 1e9
                          for index, row in enumerate(data[fs][phase])]
                result[fs][phase][role + '_median_tsc_cycles'] = statistics.median(cycles)
            row = result[fs][phase]
            row['tsc_cycles_ratio'] = row['pacha_median_tsc_cycles'] / row['linux_median_tsc_cycles']
    return result


def validate_producers(base, frozen=None):
    selected = {entry['path']: entry['sha256']
                for entry in base['installs'] + base['bootfs_inputs']}
    required = ('/lib/pacha/lpr-linux-x86_64.so', '/sbin/filed.elf',
                '/srv/kobox2/storage/core.so', '/srv/kobox2/storage/manifest.bin',
                '/srv/kobox2/storage/sandbox.elf')
    if any(path not in selected for path in required):
        raise RuntimeError('base is missing normal storage producers')
    entries = base['installs'] + base['bootfs_inputs'] + [base['kernel'], base['init_app']]
    for entry in entries:
        if digest(entry['source']) != entry['sha256']:
            raise RuntimeError('selected base artifact changed: ' + entry['source'])
    if frozen is None:
        return
    if (selected['/lib/pacha/lpr-linux-x86_64.so'] != frozen['lpr_normal']['sha256'] or
            selected.get('/sbin/lpr_supervisor.elf') != frozen['apps']['lpr_supervisor']['sha256'] or
            selected['/sbin/filed.elf'] != frozen['apps']['filed']['sha256'] or
            base['kernel']['sha256'] != frozen['kernel']['sha256']):
        raise RuntimeError('base is not the selected normal producer bundle')
    packages = json.loads(Path(frozen['packages']['source']).read_text())
    for entry in packages['artifacts']:
        if selected.get(entry['target']) != entry['sha256']:
            raise RuntimeError('base package differs from frozen bundle: ' + entry['target'])
    for group in ('apps', 'runtime', 'kernel', 'packages', 'lpr_normal',
                  'lprs_diagnostic', 'source_snapshot'):
        entries = frozen[group]
        if isinstance(entries, dict):
            entries = list(entries.values()) if group == 'apps' else [entries]
        for entry in entries:
            if digest(entry['source']) != entry['sha256']:
                raise RuntimeError('frozen producer/source changed: ' + entry['source'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mode', choices=('inspect', 'benchmark', 'stress'), required=True)
    parser.add_argument('--base-run', type=Path, required=True,
                        help='successful normal cutover run; verify its recorded producer hashes')
    parser.add_argument('--frozen', type=Path,
                        help='optional additional frozen-bundle constraints')
    parser.add_argument('--baseline', type=Path, default=ROOT / '.artifacts/fs-bench/baseline-20261002/run6')
    parser.add_argument('--iso', type=Path, default=ROOT / '.artifacts/fs-bench/baseline-20261002/alpine-virt-3.22.5-x86_64.iso')
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--iterations', type=int, default=5)
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--memory-mib', type=int, default=4096,
                        help='PachaOS workload RAM; non-default is allowed only for stress')
    args = parser.parse_args()
    if args.trials < 3 or not 3 <= args.iterations <= 100 or args.timeout <= 0:
        parser.error('require >=3 trials, 3..100 stress iterations and positive timeout')
    if not 4096 <= args.memory_mib <= 32768 or (args.mode != 'stress' and args.memory_mib != 4096):
        parser.error('require 4096..32768 MiB; benchmark/inspect must keep baseline 4096 MiB')
    args.out = args.out.resolve()
    args.base_run = args.base_run.resolve()
    args.iso = args.iso.resolve()
    args.boot_image = args.base_run / 'limine-boot.img'
    args.native_smoke = False
    args.scale = 1
    frozen = json.loads(args.frozen.read_text()) if args.frozen else None
    base = json.loads((args.base_run / 'inputs.json').read_text())
    baseline = json.loads((args.baseline / 'environment.json').read_text())
    if digest(args.iso) != baseline['iso_sha256']:
        raise RuntimeError('different Alpine ISO')
    if digest(args.boot_image) != base['boot_image_sha256']:
        raise RuntimeError('base private boot image changed')
    if digest(args.base_run / 'BOOTFS.IMG') != base['bootfs_sha256']:
        raise RuntimeError('base bootfs changed')
    if not all(json.loads((args.base_run / 'gates.json').read_text()).values()):
        raise RuntimeError('base normal cutover gates incomplete')
    validate_producers(base, frozen)
    backing = args.base_run / 'prepared.qcow2'
    chain = json.loads(subprocess.check_output(['qemu-img', 'info', '--backing-chain', '--output=json', str(backing)]))
    immutable = {str(Path(entry['filename']).resolve()): digest(entry['filename']) for entry in chain}
    immutable[str(args.boot_image)] = digest(args.boot_image)
    if immutable[str(Path(base['backing']))] != base['backing_sha256']:
        raise RuntimeError('base backing changed')
    args.out.mkdir(parents=True, exist_ok=False)
    guestdir = args.out / 'guest'
    guestdir.mkdir()
    installs = []
    def fixture(source, name, target=None):
        shutil.copyfile(source, guestdir / name)
        if target:
            index = len(installs)
            shutil.copyfile(source, guestdir / f'artifact{index:03d}')
            installs.append((target, Path(source)))
    fixture(ROOT / 'tests/fixtures/filed_nvme_inspect.sh', 'inspect.sh')
    fixture(ROOT / 'tests/fixtures/filed_nvme_fsck.sh', 'fsck.sh')
    for source in sorted((ROOT / '.artifacts/alpine-installer-apk').glob('*.apk')):
        fixture(source, source.name)
    binary = args.baseline / 'guest/bench.elf'
    if digest(binary) != baseline['binary_sha256']:
        raise RuntimeError('benchmark binary differs from baseline')
    fixture(binary, 'bench.elf', '/cmd/lpr_fs_microbench.elf')
    fixture(ROOT / 'tests/fixtures/lpr_fs_microbench.sh', 'bench.sh', '/cmd/lpr_fs_microbench.sh')
    if args.mode == 'stress':
        fixture(ROOT / 'tests/fixtures/filed_nvme_stress.sh', 'stress.sh', '/cmd/filed_nvme_stress.sh')
        fixture(ROOT / 'tests/fixtures/clang_hello.c', 'clang_hello.c', '/cmd/clang_hello.c')
    metadata = {'mode': args.mode,
        'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'host': list(platform.uname()),
        'base_run': str(args.base_run),
        'base_inputs_sha256': digest(args.base_run / 'inputs.json'),
        'base_gates_sha256': digest(args.base_run / 'gates.json'),
        'producers': base['bootfs_inputs'] + base['installs'] + [base['kernel'], base['init_app']],
        'frozen_manifest_sha256': digest(args.frozen) if args.frozen else None,
        'baseline_environment_sha256': digest(args.baseline / 'environment.json'),
        'binary_sha256': digest(binary), 'iso_sha256': digest(args.iso),
        'trials': args.trials, 'iterations': args.iterations, 'scale': args.scale,
        'pacha_workload_memory_mib': args.memory_mib,
        'immutable_inputs': immutable, 'guest_artifacts': [
            {'name': p.name, 'sha256': digest(p)} for p in sorted(guestdir.iterdir())],
        'qemu': subprocess.check_output(['qemu-system-x86_64', '--version'], text=True).splitlines()[0],
        'write_policy': 'private COW only; no rootfs copy; offline journal-only replay, no structural repair'}
    metadata['runner_sources'] = [{'path': str(p), 'sha256': digest(p)} for p in (
        Path(__file__), ROOT / 'tests/qemu_filed_cutover_smoke.py',
        ROOT / 'tests/benchmark_filed_baseline.py')]
    # Keep the exact harness bytes even when later diagnostics improve it.
    # Producer manifests alone do not make a measurement reproducible.
    runnerdir = args.out / 'runner'
    runnerdir.mkdir()
    for entry in metadata['runner_sources']:
        shutil.copyfile(entry['path'], runnerdir / Path(entry['path']).name)
    save(args.out / 'inputs.json', metadata)
    disk = None
    try:
        if args.mode == 'inspect':
            disk = args.out / 'inspect.qcow2'
            overlay(disk, backing, 'qcow2')
            linux_script(args, disk, 'inspect', 'inspect.sh', 'FILED_NVME_INSPECT_OK')
        else:
            prepared = args.out / 'prepared.qcow2'
            overlay(prepared, backing, 'qcow2')
            prepare(args, prepared, installs)
            disk = args.out / 'pacha.qcow2'
            overlay(disk, prepared, 'qcow2')
            if args.mode == 'benchmark':
                text = pacha_script(args, disk, '/cmd/lpr_fs_microbench.sh', 'FILED_NVME_BENCH_OK', f'1 {args.trials}')
                current = parse(text, args.trials)
                save(args.out / 'pacha-raw.json', current)
                native_disk = args.out / 'linux.qcow2'
                overlay(native_disk, prepared, 'qcow2')
                target_text = linux(args, native_disk, 'linux')
                native = parse(target_text, args.trials)
                save(args.out / 'linux-raw.json', native)
                clocks = validate_clocks({'pacha': text, 'linux': target_text}, args.trials)
                save(args.out / 'raw.json', {'pacha': current, 'linux': native, 'tsc_hz': clocks})
                old = json.loads((args.baseline / 'summary.json').read_text())
                save(args.out / 'summary.json', summarize(current, native, old, clocks))
            else:
                text = pacha_script(args, disk, '/cmd/filed_nvme_stress.sh', 'FILED_NVME_STRESS_OK', str(args.iterations))
                validate_stress(text, args.iterations)
                save(args.out / 'stress.json', {'apk_iterations': args.iterations, 'clang_iterations': args.iterations, 'ok': True})
    finally:
        # Always check the stopped workload disk, including failed workloads.
        # Replay in its own COW preserves the exact pre-check on-disk evidence.
        try:
            if disk is not None and disk.exists():
                checker = args.out / 'fsck.qcow2'
                overlay(checker, disk, 'qcow2')
                text = linux_script(args, checker, 'fsck', 'fsck.sh', 'FILED_NVME_FSCK_OK')
                if args.mode != 'inspect' and not 'NVME_PERSISTENCE=OK' in text:
                    # Do not conceal an earlier failed workload with a generic
                    # fsck exception. Record persistence separately, then reject
                    # a successful workload that did not leave its synced canary.
                    if (args.out / 'pacha-raw.json').exists() or (args.out / 'stress.json').exists():
                        raise RuntimeError('successful workload lost persistence canary')
                save(args.out / 'fsck.json', {'ok': True, 'final_rc': 0,
                    'before_replay_rc': int(re.search(r'NVME_FSCK_BEFORE_REPLAY_RC=(\d+)', text)[1]),
                    'journal_only_replay': True, 'structural_repairs': False,
                    'persistence_canary': 'NVME_PERSISTENCE=OK' in text})
        finally:
            checks = {p: digest(p) == h for p, h in immutable.items()}
            save(args.out / 'backing-check.json', checks)
            if not all(checks.values()):
                raise RuntimeError('immutable input changed')
    save(args.out / 'result.json', {'ok': True, 'mode': args.mode})
    print('FILED_NVME_VALIDATION_OK', args.mode, args.out, flush=True)


def validate_stress(text, iterations):
    # apk may print OK and return zero despite a failed package trigger.
    # Such a transaction is not a successful filesystem stress cycle.
    if re.search(r'(?:^|\n)ERROR:|script exited with error', text):
        raise RuntimeError('package transaction/trigger reported an error')
    for marker in ('APK_VERSION_OK', 'APK_UPDATE_OK', 'CLANG_VERSION_OK',
                   'CLANG_COMPILE_ONLY_OK', 'CLANG_LINK_RUN_OK', 'NVME_STRESS_SYNC_OK'):
        if not line_marker(marker).search(text.encode()):
            raise RuntimeError('missing stress marker: ' + marker)
    for stage in ('APK_ADD_OK', 'APK_DEL_OK', 'CLANG_ITER_OK'):
        values = re.findall(r'(?:^|\n)' + stage + r' iteration=(\d+)(?=\r?\n)', text)
        if [int(v) for v in values] != list(range(1, iterations + 1)):
            raise RuntimeError('incomplete/repeated stress cycles: ' + stage)


if __name__ == '__main__':
    main()
