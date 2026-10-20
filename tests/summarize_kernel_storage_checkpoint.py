#!/usr/bin/env python3
"""Validate the storage growth and rollback checkpoint."""
import json
from pathlib import Path
import re
import subprocess
from summarize_kernel_foundation_checkpoint import digest, validate_run, signal_marker_counts, REQUIRED_SIGNAL_MARKERS
from compare_kernel_foundation_codegen import bodies

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.artifacts/kernel-refactor-stage1'
BASE = ROOT / '.artifacts/kernel-refactor-stage0-20261011'


def validate_signal_control(directory, repeats, expected_kernel_sha256=None):
    console = (directory / 'console.log').read_text(errors='replace')
    started = re.findall(r'^OWNERSHIP_SIGNAL_TRIAL=(\d+)\r?$', console, re.M)
    completed = re.findall(r'^OWNERSHIP_SIGNAL_OK=(\d+)\r?$', console, re.M)
    expected = [str(i) for i in range(1, repeats + 1)]
    # The original single-suite controls predate the per-repeat markers.
    # Never grant that exception to a repeated run or a partially marked run.
    sequence_ok = ((repeats == 1 and not started and not completed) or
                   started == completed == expected)
    evidence = dict(expected_repeats=repeats, started=started, completed=completed,
                    caveat='Signal-only control, NOT an FS/SIMD-load reproduction.')
    if not (directory / 'result.json').exists():
        return dict(status='running-or-incomplete', strict_ok=False, **evidence)
    result = json.loads((directory / 'result.json').read_text())
    counts = signal_marker_counts(console)
    renderer = result.get('renderer', '')
    artifact_ok = (expected_kernel_sha256 is None or
                   result.get('kernel_sha256') == expected_kernel_sha256)
    passed = (result['ok'] and result.get('workload_complete') and result.get('desktop') and
              'virgl (D3D12 (' in renderer and
              not re.search('llvmpipe|softpipe', renderer, re.I) and not result['guest_fault'] and
              artifact_ok and not result.get('debugger_trace') and
              sequence_ok and all(count == repeats for count in counts.values()) and
              '=FAIL' not in console and
              re.search(r'^STAGE0_WORKLOAD_DONE\r?$', console, re.M) is not None)
    return dict(status='validated' if passed else 'failed', strict_ok=bool(passed), result=result,
                kernel_artifact_matches=artifact_ok, expected_kernel_sha256=expected_kernel_sha256,
                marker_counts=counts, console_sha256=digest(directory / 'console.log'), **evidence)


def main():
    start = json.loads((BASE / 'stage1-ownership-before-source.json').read_text())
    finish = json.loads((BASE / 'stage1-ownership-final-source.json').read_text())
    # These are the actual pre-edit observations, not the different stage-0
    # disk digest. This checkpoint may claim equality only for its own interval.
    original_start = {
        '.artifacts/disk.img': 'b36d1a22d07935b33c309cecaae1de9d47ea1b5975ae2b4bfb36b500355accac',
        '.artifacts/limine-boot.img': '92d072949b322abd823ad3eee6883ea433ea788f59aba39a07b0bc372f639a28',
    }
    original_finish = {path: digest(ROOT / path) for path in original_start}
    assert original_start == original_finish, 'Original disk/boot changed during ownership checkpoint'
    kernels = {}
    for tag in ('immutable-hooks-normal', 'immutable-hooks-debug', 'ownership-normal', 'ownership-debug'):
        path = OUT / tag / 'bin/limine/pacha-kernel.elf'
        row = subprocess.check_output(['size', str(path)], text=True).splitlines()[1].split()
        kernels[tag] = dict(sha256=digest(path), text=int(row[0]), data=int(row[1]), bss=int(row[2]))
    selected = json.loads((OUT / 'ownership-codegen.json').read_text())
    assert all(row['before_instructions'] == row['after_instructions'] and
               row['before_calls'] == row['after_calls'] for row in selected['functions'])
    old_obj, new_obj = OUT / 'pmm-raw-before.o', OUT / 'pmm-raw-after.o'
    assert digest(old_obj) == digest(new_obj), 'Raw PMM comparison objects differ'
    old_types = ROOT / '.artifacts/kernel-refactor-stage1/baseline-source-wmwrOA/kernel/src/state/types.zig'
    before_types = next(row for row in start['files'] if row['path'] == 'kernel/src/state/types.zig')
    assert before_types['sha256'] == digest(old_types), 'Fixture baseline PMM source differs from checkpoint start'
    run = validate_run(OUT / 'ownership-4g-v2')
    assert run['result']['kernel_sha256'] == kernels['ownership-debug']['sha256']
    log = (OUT / 'test-ownership-final.log').read_text()
    tests = re.search(r'(\d+)/(\d+) tests passed', log)
    assert tests and tests[1] == tests[2]
    signal_controls = {}
    for name, repeats in (('ownership-baseline-signals-4g', 1), ('ownership-new-signals-4g', 1),
                          ('ownership-baseline-signals20-4g', 20), ('ownership-new-signals20-4g', 20),
                          ('ownership-new-signals20-diag-4g', 20)):
        tag = 'immutable-hooks-debug' if 'baseline' in name else 'ownership-debug'
        signal_controls[name] = validate_signal_control(OUT / name, repeats, kernels[tag]['sha256'])
    full_comparisons = {}
    for name in ('ownership-baseline-full-4g', 'ownership-full-repeat-4g', 'ownership-baseline-full2-4g'):
        directory = OUT / name
        full_comparisons[name] = validate_run(directory) if (directory / 'result.json').exists() else dict(status='running-or-incomplete')
    report = dict(
        phase_status='active, incomplete',
        verification_status='passed' if run['strict_ok'] else 'runtime-unresolved',
        checkpoint='process/thread unpublished growth ownership and rollback',
        source_before=start['content_manifest_sha256'], source_after=finish['content_manifest_sha256'],
        tests=int(tests[1]), kernels=kernels, selected_codegen=selected,
        raw_pmm_codegen=dict(byte_identical_objects=True, sha256=digest(old_obj),
                             instructions=len(bodies(old_obj)['pmmRawAllocate']),
                             source_sha256=before_types['sha256'], optimize='ReleaseSmall, LLVM'),
        runtime=run, signal_only_controls=signal_controls, full_workload_comparisons=full_comparisons,
        original_disk_start=original_start, original_disk_finish=original_finish,
        runtime_digests_reference=str((BASE / 'bootfs-runtime-digests.json').relative_to(ROOT)),
        excluded_run=dict(name='ownership-4g', reason='Boot partition offset omitted during artifact injection; host layout rejected the old guest ELF. Not a kernel-regression test.'),
        remaining=['unresolved async epoll/signalfd signal-wait timeout; full-load repeats and signal-only repetition under investigation',
                   'published/retired borrower exclusion and reclamation',
                   'PT/VMO/boot scratch allocation/borrow/retire/reclaim APIs',
                   'common lock rank and IRQ/migration/context-switch diagnostics',
                   'all address consumers and semantic bound audit',
                   'behavior-preserving subsystem responsibility separation'],
        caveats=['KernelState constructors require fresh storage; the callsite audit is not a lifetime proof.',
                 'Signal controls pin the expected ELF digest; debugger traces are not baseline validations.',
                 'Retired arrays remain owned, not reclaimed; stable Nodes remain published.',
                 'PMM snapshots are not peak usage; static code comparisons are not latency improvements.',
                 'FS workload is six representative operation runs, not independent stress/fsck.'])
    destination = OUT / 'ownership-checkpoint-summary.json'
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(tests=report['tests'], runtime=run['status'], raw_pmm_identical=True,
                         original_disk_unchanged=True, output=str(destination)), ensure_ascii=False))
    if not run['strict_ok']:
        raise SystemExit('Recorded runtime failure; do not declare this checkpoint validated')


if __name__ == '__main__':
    main()
