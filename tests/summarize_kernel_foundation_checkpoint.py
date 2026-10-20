#!/usr/bin/env python3
"""Record evidence for this partial phase-1 checkpoint, not phase completion."""
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.artifacts/kernel-refactor-stage1'
BASE = ROOT / '.artifacts/kernel-refactor-stage0-20261011'

REQUIRED_SIGNAL_MARKERS = (
    'ASYNC_SIGINT_DEFAULT', 'ASYNC_SIGKILL', 'ASYNC_BUSYBOX_TIMEOUT',
    'ASYNC_SSE_STACK', 'ASYNC_CUSTOM_HANDLER', 'ASYNC_ALTSTACK_ONSTACK',
    'ASYNC_SIGALTSTACK', 'ASYNC_EPOLL_HANDLER', 'ASYNC_SIGNALFD_EPOLL',
    'ASYNC_SIGNALFD', 'ASYNC_EXEC_SIGIGN', 'ASYNC_EXEC_CAUGHT_RESET',
    'ASYNC_EXEC_SIGMASK', 'ASYNC_EXEC_SIGNAL_STATE', 'ASYNC_SIGTIMEDWAIT_PENDING',
    'ASYNC_SIGTIMEDWAIT_ZERO_TIMEOUT', 'ASYNC_SIGTIMEDWAIT_FINITE_TIMEOUT',
    'ASYNC_SIGTIMEDWAIT_SIGCHLD', 'ASYNC_SIGTIMEDWAIT_EINTR',
    'ASYNC_SIGTIMEDWAIT_IGNORED_RESTART', 'ASYNC_PPOLL_TIMEOUT',
    'ASYNC_PPOLL_MASK_EINTR', 'LPR_THREAD_SIGNAL_BUSY_AND_MAIN',
    'LPR_THREAD_SIGNAL_MASKED', 'LPR_THREAD_SIGNAL_MUSL_SYNCCALL',
    'LPR_THREAD_SIGNAL_FORK_EXIT', 'LPR_THREAD_SIGNAL_GROUP_KILL', 'LPR_THREAD_SIGNAL',
)


def signal_marker_counts(console):
    return {marker: len(re.findall(r'^'+marker+r'=OK(?:[ \t][^\r\n]*)?\r?$', console, re.M))
            for marker in REQUIRED_SIGNAL_MARKERS}


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def validate_run(directory, expected_kernel_sha256=None):
    result = json.loads((directory / 'result.json').read_text())
    console = (directory / 'console.log').read_text(errors='replace')
    workers = re.findall(r'SIMD_STOP_WORKER id=(\d+) iters=(\d+) bad=(\d+)', console)
    children = re.findall(r'SIMD_STOP_CHILD id=(\d+) pid=(\d+) status=(\d+)', console)
    trials = re.findall(r'STAGE0_SIMD_OK=(\d+)', console)
    blocks = re.split(r'^STAGE0_SIMD_TRIAL=(\d+)\r?$', console, flags=re.M)
    per_trial = []
    for position in range(1, len(blocks), 2):
        trial, body = blocks[position:position+2]
        trial_workers = re.findall(r'SIMD_STOP_WORKER id=(\d+) iters=(\d+) bad=(\d+)', body)
        trial_children = re.findall(r'SIMD_STOP_CHILD id=(\d+) pid=(\d+) status=(\d+)', body)
        per_trial.append(dict(trial=trial,
            valid=(sorted(worker[0] for worker in trial_workers) == ['0','1','2','3'] and
                   sorted(child[0] for child in trial_children) == ['0','1','2','3'] and
                   re.findall(r'^STAGE0_SIMD_OK=(\d+)\r?$', body, re.M) == [trial] and
                   re.findall(r'^SIMD_STOP_RC=(\d+)\r?$', body, re.M) == ['0'])))
    renderer = result.get('renderer', '')
    artifact_ok = (expected_kernel_sha256 is None or
                   result.get('kernel_sha256') == expected_kernel_sha256)
    signals = signal_marker_counts(console)
    strict_ok = (result['ok'] and result['desktop'] and result['workload_complete'] and
                 'virgl (D3D12 (' in renderer and not re.search('llvmpipe|softpipe', renderer, re.I) and
                 artifact_ok and not result.get('debugger_trace') and
                 not result['guest_fault'] and trials == [str(n) for n in range(1, 9)] and
                 [trial['trial'] for trial in per_trial] == [str(n) for n in range(1,9)] and
                 all(trial['valid'] for trial in per_trial) and
                 len(workers) == len(children) == 32 and
                 all(int(iterations) > 0 and bad == '0' for _, iterations, bad in workers) and
                 all(status == '0' for _, _, status in children) and
                 re.findall(r'^FS_BENCH_DONE fs=(\w+)\r?$', console, re.M) == ['ext4','tmpfs']*3 and
                 re.findall(r'^STAGE0_FS_TRIAL=(\d+)\r?$', console, re.M) == ['1','2','3'] and
                 all(count == 1 for count in signals.values()) and
                 re.search(r'^STAGE0_WORKLOAD_DONE\r?$', console, re.M) is not None and
                 '=FAIL' not in console)
    return dict(status='validated' if strict_ok else 'failed', strict_ok=strict_ok,
                result=result, simd_trials=len(trials), worker_results=len(workers),
                per_trial=per_trial, signal_marker_counts=signals,
                kernel_artifact_matches=artifact_ok,
                child_results=len(children), fs_runs=console.count('FS_BENCH_DONE fs='),
                console_sha256=digest(directory / 'console.log'),
                serial_sha256=digest(directory / 'serial.log'))


def main():
    old_disk = json.loads((BASE / 'backing-before.json').read_text())
    disk = dict(disk_sha256=digest(ROOT / '.artifacts/disk.img'),
                boot_sha256=digest(ROOT / '.artifacts/limine-boot.img'))
    source_before = json.loads((BASE / 'stage1-before-source.json').read_text())
    source_start = datetime.fromisoformat(source_before['utc']).timestamp()
    disk_mtime = (ROOT / '.artifacts/disk.img').stat().st_mtime
    disk_observation = dict(current=disk, stage0_reference=old_disk,
        matches_stage0=disk == old_disk, phase1_start_digest=None,
        disk_mtime_utc=datetime.fromtimestamp(disk_mtime, timezone.utc).isoformat(),
        phase1_source_start_utc=source_before['utc'],
        disk_mtime_predates_phase1_start=disk_mtime < source_start,
        caveat='Stage-0 disk digest differs; disk mtime predates this phase. No phase-1 start disk digest was captured. Commands write only per-run COW/boot artifacts; do not claim before/after digest equality.')
    if disk_mtime >= source_start:
        raise SystemExit('Original disk mtime changed during this phase; investigate')
    kernels = {}
    for tag in ('before-normal', 'before-reproduced-normal', 'immutable-hooks-normal', 'immutable-hooks-debug'):
        path = OUT / tag / 'bin/limine/pacha-kernel.elf'
        row = subprocess.check_output(['size', str(path)], text=True).splitlines()[1].split()
        kernels[tag] = dict(path=str(path.relative_to(ROOT)), sha256=digest(path),
                           text=int(row[0]), data=int(row[1]), bss=int(row[2]))
    if kernels['before-normal']['sha256'] != kernels['before-reproduced-normal']['sha256']:
        raise SystemExit('Reconstructed dirty baseline is not byte-identical')
    runs = {}
    for name in ('address-4g', 'immutable-hooks-4g'):
        directory = OUT / name
        result_file = directory / 'result.json'
        if not result_file.exists():
            runs[name] = dict(status='running-or-incomplete')
            continue
        runs[name] = validate_run(directory)
    report = dict(phase_status='active, incomplete', kernels=kernels, original_disk_observation=disk_observation,
        backing_chain=[dict(path=str(path.relative_to(ROOT)), sha256=digest(path)) for path in
            (BASE / 'prepared-clean.qcow2', BASE / 'prepared.qcow2')],
        runtime_digests_reference=str((BASE / 'bootfs-runtime-digests.json').relative_to(ROOT)),
        source_reference=str((BASE / 'stage1-address-final-source.json').relative_to(ROOT)),
        selected_codegen=json.loads((OUT / 'immutable-hooks-codegen.json').read_text()), runs=runs,
        remaining=['all address consumers', 'growth rollback and published storage reclamation',
                   'PT/VMO/boot scratch borrowing APIs', 'common lock rank / IRQ / migration / switch checks',
                   'full semantic bound and subsystem audit', 'behavior-preserving responsibility separation',
                   'remaining value Hooks snapshots copy eight additional bytes'],
        caveats=['FS workload validates operation, not paired latency improvement',
                 'PMM checkpoints and RSP samples are not workload peaks/high-water'])
    (OUT / 'address-checkpoint-summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(disk_mtime_predates_phase1_start=True, matches_stage0_disk=disk == old_disk,
                          runs={name: row['status'] for name, row in runs.items()},
                          output=str(OUT / 'address-checkpoint-summary.json')), ensure_ascii=False))


if __name__ == '__main__':
    main()
