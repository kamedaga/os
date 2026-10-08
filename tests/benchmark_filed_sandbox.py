#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure IPC-free Linux FS execution inside the actual storage sandbox."""

import argparse
import itertools
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess

from benchmark_filed_baseline import overlay
from validate_filed_nvme import digest, linux_script, save, validate_producers

ROOT = Path(__file__).resolve().parents[1]
BENCH = re.compile(r'^FS_INTERNAL_BENCH fs=(ext4|tmpfs) opcode=(\d+) depth=(0|1|8) '
                   r'trial=([123]) ops=(\d+) ticks=(\d+) elapsed_ns=(\d+) task_ns=(\d+)\r?$', re.M)
SERVICE = re.compile(r'^FS_SERVICE_PROFILE opcode=(\d+) calls=(\d+) snapshot=(\d+) '
                     r'auth=(\d+) execute=(\d+) release=(\d+) task_work=(\d+) queue=(\d+)\r?$', re.M)
WORKER = re.compile(r'^FS_WORKER_PROFILE calls=(\d+) submit=(\d+) release=(\d+)\r?$', re.M)
WORKER_PATH = re.compile(r'^FS_WORKER_PATH calls=(\d+) submit_nonwake=(\d+) '
                         r'ready_wake=(\d+) completion_to_collect=(\d+)\r?$', re.M)
SWITCHES = re.compile(r'^FS_INTERNAL_SWITCHES switches=(\d+)\r?$', re.M)
BOUNDARIES = re.compile(r'^FS_INTERNAL_BOUNDARIES preempt=(\d+) irq=(\d+) '
                        r'clock=(\d+) handoff=(\d+)\r?$', re.M)
META = re.compile(r'^FS_WORKER_META new_work=(\d+) reused_work=(\d+) '
                  r'new_client=(\d+) reused_client=(\d+)\r?$', re.M)
PLACEMENT = re.compile(r'^FS_WORKER_PLACEMENT inline=(\d+) woken=(\d+) queued=(\d+) empty=(\d+)\r?$', re.M)
BUFFER = re.compile(r'^FS_WORKER_BUFFER new=(\d+) reused=(\d+)\r?$', re.M)
GUARD = re.compile(r'^FS_INTERNAL_GUARD kind=(\w+) trial=(\d+) ops=(\d+) '
                   r'ticks=(\d+) elapsed_ns=(\d+) task_ns=(\d+)\r?$', re.M)
NATIVE_PATTERNS = {
    'HANDOFF': (re.compile(r'^FS_NATIVE_HANDOFF count=(\d+) publish_ticks=(\d+) '
                           r'enter_ticks=(\d+) enter_waits=(\d+)\r?$', re.M),
                ('handoffs', 'publish_ticks', 'enter_ticks', 'enter_waits')),
    'WAKE': (re.compile(r'^FS_NATIVE_WAKE calls=(\d+) ticks=(\d+)\r?$', re.M),
             ('wake_calls', 'wake_ticks')),
    'PARK': (re.compile(r'^FS_NATIVE_PARK calls=(\d+) fast=(\d+) waits=(\d+) '
                        r'wait_ticks=(\d+)\r?$', re.M),
             ('park_calls', 'park_fast', 'park_waits', 'park_wait_ticks')),
    'OWNER_WAIT': (re.compile(r'^FS_NATIVE_OWNER_WAIT calls=(\d+) ticks=(\d+)\r?$', re.M),
                   ('owner_wait_calls', 'owner_wait_ticks')),
}
NATIVE_DETAIL_PATTERNS = {
    'RESUME': (re.compile(r'^FS_NATIVE_RESUME count=(\d+) ticks=(\d+) '
                          r'tail_ticks=(\d+) missing=(\d+)\r?$', re.M),
               ('resume_count', 'resume_ticks', 'resume_tail_ticks', 'resume_missing')),
    'LOCAL': (re.compile(r'^FS_NATIVE_LOCAL count=(\d+) ticks=(\d+)\r?$', re.M),
              ('local_count', 'local_ticks')),
    'REMOTE': (re.compile(r'^FS_NATIVE_REMOTE count=(\d+) ticks=(\d+)\r?$', re.M),
               ('remote_count', 'remote_ticks')),
}


def parse_guards(text):
    text = re.sub(r'(?m)^\[Thread \d+\] ', '', text)
    rows = [{'kind': m[1], 'trial': int(m[2]), 'ops': int(m[3]),
             'ticks': int(m[4]), 'elapsed_ns': int(m[5]), 'linux_task_ns': int(m[6])}
            for m in GUARD.finditer(text)]
    if not rows:
        if 'FS_INTERNAL_GUARD' in text:
            raise RuntimeError('truncated guard diagnostics')
        return []
    kinds = {r['kind'] for r in rows}
    if kinds not in ({'irq', 'preempt'}, {'irq', 'preempt', 'clock', 'fp'}):
        raise RuntimeError('unknown/partial primitive diagnostics')
    expected = set(itertools.product(kinds, (1, 2, 3)))
    keys = [(r['kind'], r['trial']) for r in rows]
    if len(keys) != len(expected) or set(keys) != expected or any(
            r['ops'] <= 0 or r['ticks'] <= 0 or r['elapsed_ns'] < 1000000000 for r in rows):
        raise RuntimeError('incomplete/duplicate/short guard diagnostics')
    return rows


def check_runtime_flags(flags, verify_only, elapsed_only=False, handoff_only=False, publication_only=False, executor_only=False):
    bench = '-DKOBOX_FS_INTERNAL_BENCH=1' in flags
    profile = '-DKOBOX_FS_SERVICE_PROFILE=1' in flags
    lean = '-DKOBOX_FS_BENCH_ELAPSED_ONLY=1' in flags
    handoff = '-DKOBOX_FS_HANDOFF_BENCH_ONLY=1' in flags
    publication = '-DKOBOX_FS_PUBLICATION_BENCH=1' in flags
    executor = '-DKOBOX_FS_EXECUTOR_BENCH=1' in flags
    if executor != executor_only or (executor_only and (not publication_only or '-DKOBOX_FS_INLINE_DISABLED=1' not in flags)):
        raise RuntimeError('executor flags require publication workload and worker-only control')
    if publication != publication_only or (publication_only and (not elapsed_only or verify_only or handoff_only)):
        raise RuntimeError('publication flags do not match measurement mode')
    if handoff != handoff_only or (handoff_only and (not elapsed_only or verify_only)):
        raise RuntimeError('handoff-only flags do not match measurement mode')
    if '-DKOBOX_BOOT_FS=1' not in flags or '-DKOBOX_RUNTIME_GATES=1' in flags:
        raise RuntimeError('require production storage runtime, not gate configuration')
    if (verify_only and (bench or profile or lean or elapsed_only)) or (
            not verify_only and (not bench or lean != elapsed_only or profile == elapsed_only)):
        raise RuntimeError('diagnostic flags do not match benchmark/normal verification mode')
    if elapsed_only and any(flag.startswith('-DKOBOX_FS_BOUNDARY_CALLERS') for flag in flags):
        raise RuntimeError('elapsed-only artifact contains boundary attribution probes')


def parse_handoff(text):
    text = re.sub(r'(?m)^\[Thread \d+\] ', '', text)
    pattern = re.compile(r'^FS_INTERNAL_HANDOFF cpu=(\d+) trial=([123]) rounds=(\d+) '
                         r'ticks=(\d+) elapsed_ns=(\d+) switches=(\d+)\r?$', re.M)
    rows = [dict(zip(('cpu', 'trial', 'rounds', 'ticks', 'elapsed_ns', 'switches'),
                     map(int, m.groups()))) for m in pattern.finditer(text)]
    if len(rows) != 3 or {r['trial'] for r in rows} != {1, 2, 3} or len({r['cpu'] for r in rows}) != 1:
        raise RuntimeError('incomplete/duplicate handoff-only trials')
    if any(r['rounds'] <= 0 or r['ticks'] <= 0 or r['elapsed_ns'] < 1000000000 or
           r['switches'] < 2 * r['rounds'] - 2 for r in rows):
        raise RuntimeError('short or nonblocking handoff-only workload')
    if re.search(r'^FS_(?:INTERNAL_BENCH|INTERNAL_BOUNDARIES|SERVICE_PROFILE|WORKER_|NATIVE_)', text, re.M):
        raise RuntimeError('handoff-only log contains filesystem workload or hot probes')
    if not re.search(r'^FS_INTERNAL_HANDOFF_DONE status=0\r?$', text, re.M):
        raise RuntimeError('handoff-only tasks did not quiesce successfully')
    return rows


def parse(text, opcodes, elapsed_only=False):
    text = re.sub(r'(?m)^\[Thread \d+\] ', '', text)
    # Never interpret absent probes as zero cost, or silently accept a truncated
    # detailed run as a lean one. The caller must select the expected mode.
    if elapsed_only and re.search(r'^FS_(?:SERVICE_PROFILE|WORKER_(?:PROFILE|PATH|META|PLACEMENT|BUFFER)|'
                                  r'INTERNAL_BOUNDARIES|BOUNDARY_SITE|NATIVE_)', text, re.M):
        raise RuntimeError('elapsed-only log contains detailed profiling records')
    rows = []
    for match in BENCH.finditer(text):
        fs, opcode, depth, trial, ops, ticks, elapsed, task = match.groups()
        opcode, depth, trial, ops = map(int, (opcode, depth, trial, ops))
        end = BENCH.search(text, match.end())
        tail = text[match.end():end.start() if end else len(text)]
        profiles = [p for p in SERVICE.finditer(tail) if int(p[1]) == opcode]
        if not elapsed_only and (not profiles or int(profiles[0][2]) != ops):
            raise RuntimeError('service count does not match timed operations')
        if opcode not in opcodes or ops <= 0 or int(elapsed) < 1000000000 or int(ticks) <= 0:
            raise RuntimeError('unknown opcode, empty or short measurement')
        row = {'fs': fs, 'phase': opcodes[opcode], 'depth': depth, 'trial': trial,
               'ops': ops, 'ticks': int(ticks), 'elapsed_ns': int(elapsed),
               'linux_task_ns': int(task)}
        if not elapsed_only:
            row['stages'] = dict(zip(('snapshot', 'auth', 'execute', 'release', 'task_work', 'queue'),
                                     map(int, profiles[0].groups()[2:])))
        switches = SWITCHES.findall(tail)
        if switches:
            if len(switches) != 1:
                raise RuntimeError('duplicate task switch diagnostic')
            row['linux_task_switches'] = int(switches[0])
        elif 'FS_INTERNAL_SWITCHES' in tail:
            raise RuntimeError('truncated task switch diagnostic')
        if elapsed_only:
            rows.append(row)
            continue
        boundaries = BOUNDARIES.findall(tail)
        if boundaries:
            if len(boundaries) != 1:
                raise RuntimeError('duplicate host boundary diagnostics')
            row['boundary_calls'] = dict(zip(('preempt', 'irq', 'clock', 'handoff'),
                                             map(int, boundaries[0])))
        elif 'FS_INTERNAL_BOUNDARIES' in tail:
            raise RuntimeError('truncated host boundary diagnostics')
        if re.search(r'^FS_NATIVE_(?:HANDOFF|WAKE|PARK|OWNER_WAIT|RESUME|LOCAL|REMOTE)', tail, re.M):
            native = {}
            for tag, (pattern, labels) in NATIVE_PATTERNS.items():
                records = pattern.findall(tail)
                if len(records) != 1:
                    raise RuntimeError('incomplete/duplicate native handoff diagnostics: ' + tag)
                native.update(zip(labels, map(int, records[0])))
            if re.search(r'^FS_NATIVE_(?:RESUME|LOCAL|REMOTE)', tail, re.M):
                for tag, (pattern, labels) in NATIVE_DETAIL_PATTERNS.items():
                    records = pattern.findall(tail)
                    if len(records) != 1:
                        raise RuntimeError('incomplete/duplicate native handoff details: ' + tag)
                    native.update(zip(labels, map(int, records[0])))
                if native['resume_count'] + native['resume_missing'] != native['handoffs']:
                    raise RuntimeError('resume counts do not match handoffs')
                # Unsupported RDTSCP produces no locality counts; do not label
                # those handoffs as CPU-local or silently infer a CPU number.
                classified = native['local_count'] + native['remote_count']
                locality_ticks = native['local_ticks'] + native['remote_ticks']
                if classified not in (0, native['handoffs']) or (
                        classified and locality_ticks != native['publish_ticks']) or (
                        not classified and locality_ticks):
                    raise RuntimeError('locality details do not match handoffs')
                if not native['resume_missing'] and (
                        native['resume_ticks'] + native['resume_tail_ticks'] != native['publish_ticks']):
                    raise RuntimeError('resume intervals do not match handoff interval')
            row['native'] = native
            combined = re.findall(r'^FS_NATIVE_COMBINED calls=(\d+) ticks=(\d+)\r?$', tail, re.M)
            if 'FS_NATIVE_COMBINED' in tail:
                if len(combined) != 1:
                    raise RuntimeError('incomplete/duplicate combined native diagnostics')
                native.update(zip(('combined_calls', 'combined_ticks'), map(int, combined[0])))
        if depth:
            worker = WORKER.search(tail)
            if not worker or int(worker[1]) != ops:
                raise RuntimeError('worker count does not match timed operations')
            row['owner'] = {'submit': int(worker[2]), 'release': int(worker[3])}
            paths = WORKER_PATH.findall(tail)
            if paths or 'FS_WORKER_PATH' in tail:
                if len(paths) != 1 or int(paths[0][0]) != ops:
                    raise RuntimeError('incomplete/duplicate/wrong worker path counts')
                nonwake, wake, completion = map(int, paths[0][1:])
                if nonwake + wake != row['owner']['submit']:
                    raise RuntimeError('worker wake intervals do not match submit interval')
                row['worker_path'] = dict(zip(('submit_nonwake', 'ready_wake', 'completion_to_collect'),
                                              (nonwake, wake, completion)))
            buffers = BUFFER.findall(tail)
            if buffers or 'FS_WORKER_BUFFER' in tail:
                if len(buffers) != 1 or sum(map(int, buffers[0])) != ops:
                    raise RuntimeError('incomplete/duplicate/wrong buffer counts')
                row['buffers'] = dict(zip(('new', 'reused'), map(int, buffers[0])))
            placement = PLACEMENT.findall(tail)
            if placement or 'FS_WORKER_PLACEMENT' in tail:
                if len(placement) != 1:
                    raise RuntimeError('incomplete/duplicate/wrong placement counts')
                counts = dict(zip(('inline', 'woken', 'queued', 'empty'), map(int, placement[0])))
                if (counts['inline'] + counts['woken'] + counts['queued'] != ops
                        or counts['empty'] > counts['woken']):
                    raise RuntimeError('incomplete/duplicate/wrong placement counts')
                row['placement'] = counts
            metadata = META.findall(tail)
            if metadata:
                if len(metadata) != 1:
                    raise RuntimeError('duplicate metadata diagnostics')
                new_work, reused_work, new_client, reused_client = map(int, metadata[0])
                if new_work + reused_work != ops or new_client + reused_client > ops:
                    raise RuntimeError('metadata counts do not match submitted operations')
                row['metadata'] = dict(zip(('new_work', 'reused_work', 'new_client', 'reused_client'),
                                           (new_work, reused_work, new_client, reused_client)))
        rows.append(row)
    expected = set(itertools.product(('ext4', 'tmpfs'), opcodes.values(), (0, 1, 8), (1, 2, 3)))
    keys = [(r['fs'], r['phase'], r['depth'], r['trial']) for r in rows]
    if len(keys) != len(expected) or set(keys) != expected:
        raise RuntimeError('incomplete/duplicate benchmark trials')
    if any('linux_task_switches' in r for r in rows) and not all('linux_task_switches' in r for r in rows):
        raise RuntimeError('partial task switch diagnostics')
    worker_rows = [r for r in rows if r['depth']]
    if any('buffers' in r for r in worker_rows) and not all('buffers' in r for r in worker_rows):
        raise RuntimeError('partial buffer diagnostics')
    if any('placement' in r for r in worker_rows) and not all('placement' in r for r in worker_rows):
        raise RuntimeError('partial placement diagnostics')
    if any('native' in r for r in rows) and not all('native' in r for r in rows):
        raise RuntimeError('partial native handoff diagnostics')
    if any('combined_calls' in r.get('native', {}) for r in rows) and not all(
            'combined_calls' in r.get('native', {}) for r in rows):
        raise RuntimeError('partial combined native diagnostics')
    if any('resume_count' in r.get('native', {}) for r in rows) and not all(
            'resume_count' in r.get('native', {}) for r in rows):
        raise RuntimeError('partial native handoff details')
    if any('worker_path' in r for r in worker_rows) and not all('worker_path' in r for r in worker_rows):
        raise RuntimeError('partial worker path diagnostics')
    if any('boundary_calls' in r for r in rows) and not all('boundary_calls' in r for r in rows):
        raise RuntimeError('partial host boundary diagnostics')
    if any('metadata' in r for r in worker_rows) and not all('metadata' in r for r in worker_rows):
        raise RuntimeError('partial metadata diagnostics')
    if not re.search(r'^FS_INTERNAL_BENCH_DONE status=0\r?$', text, re.M):
        raise RuntimeError('benchmark did not quiesce successfully')
    return rows


def summarize(rows):
    result = {}
    for fs, phase, depth in itertools.product(('ext4', 'tmpfs'), ('stat', 'pread', 'pwrite'), (0, 1, 8)):
        group = [r for r in rows if (r['fs'], r['phase'], r['depth']) == (fs, phase, depth)]
        median = lambda values: statistics.median(values)
        row = {'ticks_per_op': median(r['ticks'] / r['ops'] for r in group),
               'elapsed_ns_per_op': median(r['elapsed_ns'] / r['ops'] for r in group),
               'linux_task_ns_per_op': median(r['linux_task_ns'] / r['ops'] for r in group),
               'ops_per_second': median(r['ops'] * 1e9 / r['elapsed_ns'] for r in group)}
        if all('stages' in r for r in group):
            row['stage_ticks_per_op'] = {stage: median(r['stages'][stage] / r['ops'] for r in group)
                                         for stage in group[0]['stages']}
        if depth and all('owner' in r for r in group):
            row['owner_ticks_per_op'] = {stage: median(r['owner'][stage] / r['ops'] for r in group)
                                         for stage in ('submit', 'release')}
        if depth and all('worker_path' in r for r in group):
            row['worker_path_ticks_per_op'] = {key: median(r['worker_path'][key] / r['ops'] for r in group)
                                               for key in group[0]['worker_path']}
        if depth and all('buffers' in r for r in group):
            row['buffers_per_op'] = {key: median(r['buffers'][key] / r['ops'] for r in group)
                                     for key in ('new', 'reused')}
        if all('linux_task_switches' in r for r in group):
            row['linux_task_switches_per_op'] = median(r['linux_task_switches'] / r['ops']
                                                      for r in group)
        if depth and all('metadata' in r for r in group):
            row['metadata_per_op'] = {key: median(r['metadata'][key] / r['ops'] for r in group)
                                      for key in group[0]['metadata']}
        if depth and all('placement' in r for r in group):
            row['placement_per_op'] = {key: median(r['placement'][key] / r['ops'] for r in group)
                                       for key in ('inline', 'woken', 'queued', 'empty')}
        if all('boundary_calls' in r for r in group):
            row['boundary_calls_per_op'] = {key: median(r['boundary_calls'][key] / r['ops'] for r in group)
                                            for key in group[0]['boundary_calls']}
        if all('native' in r for r in group):
            row['native_per_op'] = {key: median(r['native'][key] / r['ops'] for r in group)
                                    for key in group[0]['native']}
            if all(r['native']['handoffs'] for r in group):
                row['native_publish_ticks_per_handoff'] = median(
                    r['native']['publish_ticks'] / r['native']['handoffs'] for r in group)
        result.setdefault(fs, {}).setdefault(phase, {})[str(depth)] = row
    return result


def parse_publication(text, opcodes, executor_only=False):
    text = re.sub(r'(?m)^\[Thread \d+\] ', '', text)
    pattern = re.compile(r'^FS_INTERNAL_PUBLICATION fs=(ext4|tmpfs) opcode=(\d+) depth=(1|8) '
        r'trial=([123]) mode=(owner|worker|executor) ops=(\d+) ticks=(\d+) elapsed_ns=(\d+) '
        r'publication_ticks=(\d+) copy_ticks=(\d+) retirement_ticks=(\d+) '
        r'worker_publications=(\d+) switches=(\d+)\r?$', re.M)
    rows = []
    for match in pattern.finditer(text):
        fs, opcode, depth, trial, mode, *values = match.groups()
        if int(opcode) not in opcodes:
            raise RuntimeError('unknown publication opcode')
        row = dict(zip(('ops', 'ticks', 'elapsed_ns', 'publication_ticks', 'copy_ticks',
                        'retirement_ticks', 'worker_publications', 'switches'), map(int, values)))
        end = pattern.search(text, match.end())
        tail = text[match.end():end.start() if end else len(text)]
        counts = re.findall(r'^FS_INTERNAL_COMPLETION wake_calls=(\d+) batches=(\d+)\r?$', tail, re.M)
        if counts or 'FS_INTERNAL_COMPLETION' in tail:
            if len(counts) != 1 or tail.count('FS_INTERNAL_COMPLETION') != 1:
                raise RuntimeError('invalid completion accounting record')
            row.update(completion_wake_calls=int(counts[0][0]), collection_batches=int(counts[0][1]))
            if row['completion_wake_calls'] > row['ops'] or row['collection_batches'] > row['ops']:
                raise RuntimeError('completion accounting exceeds operation count')
        row.update(fs=fs, phase=opcodes[int(opcode)], depth=int(depth), trial=int(trial), mode=mode)
        if (row['ops'] <= 0 or row['ticks'] <= 0 or row['elapsed_ns'] < 1000000000 or
                row['worker_publications'] > row['ops'] or
                (mode == 'owner' and row['worker_publications']) or
                row['copy_ticks'] > row['publication_ticks']):
            raise RuntimeError('invalid/short publication measurement')
        rows.append(row)
    modes = ('worker', 'executor') if executor_only else ('owner', 'worker')
    expected = set(itertools.product(('ext4', 'tmpfs'), opcodes.values(), (1, 8), (1, 2, 3), modes))
    keys = [(r['fs'], r['phase'], r['depth'], r['trial'], r['mode']) for r in rows]
    if len(keys) != len(expected) or set(keys) != expected or text.count('FS_INTERNAL_PUBLICATION') != len(rows):
        raise RuntimeError('incomplete/duplicate publication trials')
    if any('completion_wake_calls' in r for r in rows) and not all('completion_wake_calls' in r for r in rows):
        raise RuntimeError('partial completion accounting')
    if executor_only and any(r['worker_publications'] != r['ops'] or
            'collection_batches' not in r or (r['mode'] == 'executor' and r['collection_batches']) for r in rows):
        raise RuntimeError('executor comparison requires worker publication and no owner collection')
    if executor_only and sorted(re.findall(r'^FS_EXECUTOR_CHECKS fs=(ext4|tmpfs) status=0\r?$', text, re.M)) != ['ext4', 'tmpfs']:
        raise RuntimeError('executor lifecycle checks missing or duplicated')
    if not re.search(r'^FS_INTERNAL_BENCH_DONE status=0\r?$', text, re.M):
        raise RuntimeError('publication benchmark did not quiesce')
    if re.search(r'^FS_(?:INTERNAL_BENCH |SERVICE_PROFILE|WORKER_PROFILE|NATIVE_|INTERNAL_BOUNDARIES)', text, re.M):
        raise RuntimeError('unexpected workload/profiling in publication benchmark')
    return rows


def summarize_publication(rows, executor_only=False):
    result = {}
    for fs, phase, depth, mode in itertools.product(('ext4', 'tmpfs'), ('stat', 'pread', 'pwrite'), (1, 8),
                                                  ('worker', 'executor') if executor_only else ('owner', 'worker')):
        group = [r for r in rows if (r['fs'], r['phase'], r['depth'], r['mode']) == (fs, phase, depth, mode)]
        data = {key.replace('_ticks', '_ns_per_request'): statistics.median(
                    r[key] / r['ops'] * r['elapsed_ns'] / r['ticks'] for r in group)
                for key in ('publication_ticks', 'copy_ticks', 'retirement_ticks')}
        data.update(elapsed_ns_per_op=statistics.median(r['elapsed_ns'] / r['ops'] for r in group),
                    ops_per_second=statistics.median(r['ops'] * 1e9 / r['elapsed_ns'] for r in group),
                    worker_publication_fraction=statistics.median(r['worker_publications'] / r['ops'] for r in group),
                    switches_per_op=statistics.median(r['switches'] / r['ops'] for r in group))
        if all('completion_wake_calls' in r for r in group):
            for key in ('completion_wake_calls', 'collection_batches'):
                data[key + '_per_op'] = statistics.median(r[key] / r['ops'] for r in group)
        result.setdefault(fs, {}).setdefault(phase, {}).setdefault(str(depth), {})[mode] = data
    return result


def collect(out, elapsed_only=False, handoff_only=False, publication_only=False, executor_only=False):
    header = (ROOT / 'kobox2/protocol/generated/include/kobox2/filesystem_layout.h').read_text()
    opcodes = {int(re.search(r'#define KB2_FILESYSTEM_OP_' + name + r' (\d+)u', header)[1]): phase
               for name, phase in (('STATX', 'stat'), ('PREAD', 'pread'), ('PWRITE', 'pwrite'))}
    log = (out / 'qemu/pacha.log').read_text()
    rows = parse_publication(log, opcodes, executor_only) if publication_only else parse_handoff(log) if handoff_only else parse(log, opcodes, elapsed_only)
    guards = parse_guards(log)
    if (out / 'raw.json').exists() or (out / 'summary.json').exists():
        raise RuntimeError('refuse to overwrite collected measurement evidence')
    save(out / 'raw.json', rows)
    save(out / 'summary.json', {
        'elapsed_ns_per_roundtrip': statistics.median(r['elapsed_ns'] / r['rounds'] for r in rows),
        'ticks_per_roundtrip': statistics.median(r['ticks'] / r['rounds'] for r in rows),
        'switches_per_roundtrip': statistics.median(r['switches'] / r['rounds'] for r in rows),
        'nominal_transfer_ns': statistics.median(r['elapsed_ns'] / (2 * r['rounds']) for r in rows),
        'boundary': 'two same-logical-CPU Linux tasks; completion/wait plus native task transfer; no FS',
    } if handoff_only else summarize_publication(rows, executor_only) if publication_only else summarize(rows))
    if guards:
        save(out / 'guards-raw.json', guards)
        save(out / 'guards.json', {kind: {
            f'ticks_per_{unit}': statistics.median(r['ticks'] / r['ops'] for r in guards if r['kind'] == kind),
            f'elapsed_ns_per_{unit}': statistics.median(r['elapsed_ns'] / r['ops'] for r in guards if r['kind'] == kind),
            f'linux_task_ns_per_{unit}': statistics.median(r['linux_task_ns'] / r['ops'] for r in guards if r['kind'] == kind),
        } for kind in sorted({r['kind'] for r in guards})
            for unit in ('pair' if kind in ('irq', 'preempt') else 'call',)})
    save(out / 'collector.json', {'source_sha256': digest(Path(__file__)),
                                  'log_sha256': digest(out / 'qemu/pacha.log'),
                                  'elapsed_only': elapsed_only, 'handoff_only': handoff_only,
                                  'publication_only': publication_only, 'executor_only': executor_only})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--base-run', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, default=ROOT / '.artifacts/kobox2-storage-production/closure-inventory.json')
    parser.add_argument('--iso', type=Path, default=ROOT / '.artifacts/fs-bench/baseline-20261002/alpine-virt-3.22.5-x86_64.iso')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--adapter', type=Path,
                        help='explicit native storage adapter ELF; default is verified base adapter')
    parser.add_argument('--kernel', type=Path,
                        help='explicit kernel ELF for controlled kernel-only comparisons')
    parser.add_argument('--collect-only', action='store_true',
                        help='audit preserved raw log without rerunning or replacing evidence')
    parser.add_argument('--verify-only', action='store_true',
                        help='verify normal diagnostic-OFF artifact and fsck, without measurement')
    parser.add_argument('--elapsed-only', action='store_true',
                        help='require total-time benchmark with detailed core and adapter probes OFF')
    parser.add_argument('--handoff-only', action='store_true',
                        help='require same-logical-CPU task roundtrip workload, without FS operations')
    parser.add_argument('--publication-only', action='store_true',
                        help='compare sandbox-private owner/worker publication; no client workload')
    parser.add_argument('--executor-only', action='store_true',
                        help='compare private executor intake/recycle against worker submit/collect')
    args = parser.parse_args()
    if args.collect_only and args.verify_only:
        parser.error('--collect-only and --verify-only are mutually exclusive')
    if args.elapsed_only and args.verify_only:
        parser.error('--elapsed-only is a measurement mode, not normal verification')
    if args.handoff_only and not args.elapsed_only:
        parser.error('--handoff-only requires --elapsed-only')
    if args.publication_only and (not args.elapsed_only or args.verify_only or args.handoff_only):
        parser.error('--publication-only requires --elapsed-only and excludes other workloads')
    if args.executor_only and not args.publication_only:
        parser.error('--executor-only requires --publication-only')
    for name in ('runtime', 'out', 'base_run', 'inventory', 'iso'):
        setattr(args, name, getattr(args, name).resolve())
    if args.adapter:
        args.adapter = args.adapter.resolve(strict=True)
    if args.kernel:
        args.kernel = args.kernel.resolve(strict=True)
    if args.collect_only:
        if json.loads((args.out / 'inputs.json').read_text()).get('elapsed_only', False) != args.elapsed_only:
            raise RuntimeError('collection mode does not match preserved run')
        if json.loads((args.out / 'inputs.json').read_text()).get('handoff_only', False) != args.handoff_only:
            raise RuntimeError('collection workload does not match preserved run')
        if json.loads((args.out / 'inputs.json').read_text()).get('publication_only', False) != args.publication_only:
            raise RuntimeError('collection publication mode does not match preserved run')
        if json.loads((args.out / 'inputs.json').read_text()).get('executor_only', False) != args.executor_only:
            raise RuntimeError('collection executor mode does not match preserved run')
        if not all(json.loads((args.out / 'qemu/gates.json').read_text()).values()):
            raise RuntimeError('normal cutover gates failed')
        if json.loads((args.out / 'fsck.json').read_text())['final_rc']:
            raise RuntimeError('offline fsck failed')
        collect(args.out, args.elapsed_only, args.handoff_only, args.publication_only, args.executor_only)
        print('FS_SANDBOX_COLLECTION_OK', args.out, flush=True)
        return
    base = json.loads((args.base_run / 'inputs.json').read_text())
    validate_producers(base)
    if not all(json.loads((args.base_run / 'gates.json').read_text()).values()):
        raise RuntimeError('base normal cutover is not verified')
    flags = json.loads((args.runtime / 'linux-boot-inputs.json').read_text())['extra_cflags']
    check_runtime_flags(flags, args.verify_only, args.elapsed_only, args.handoff_only, args.publication_only, args.executor_only)
    args.out.mkdir(parents=True, exist_ok=False)
    (args.out / 'guest').mkdir()
    shutil.copyfile(ROOT / 'tests/fixtures/filed_nvme_fsck.sh', args.out / 'guest/fsck.sh')
    for source in (ROOT / '.artifacts/alpine-installer-apk').glob('*.apk'):
        shutil.copyfile(source, args.out / 'guest' / source.name)
    package = args.out / 'package'
    subprocess.run(['python3', str(ROOT / 'kobox2/linux-sandbox/kobox/manifest/build_pci_package.py'),
                    '--inventory', str(args.inventory), '--runtime-dir', str(args.runtime),
                    '--output-dir', str(package), '--protocol-dir', str(ROOT / 'kobox2/protocol'),
                    '--profile', 'storage', '--generator-dir', str(args.out / 'manifest-build')], check=True)
    selected = {r['path']: r['source'] for r in base['bootfs_inputs']}
    adapter = args.adapter or Path(selected['/srv/kobox2/storage/sandbox.elf'])
    if args.elapsed_only:
        symbols = subprocess.check_output(['nm', '-a', str(adapter)], text=True)
        if re.search(r'\b(?:handoff_trace|handoff_note|ph_handoff_\w+)\b', symbols):
            raise RuntimeError('elapsed-only adapter contains handoff instrumentation')
    kernel = args.kernel or Path(base['kernel']['source'])
    shutil.copyfile(adapter, package / 'sandbox.elf')
    command = ['python3', str(ROOT / 'tests/qemu_filed_cutover_smoke.py'),
               '--out', str(args.out / 'qemu'), '--iso', str(args.iso),
               '--backing', str(args.base_run / 'prepared.qcow2'),
               '--kernel', str(kernel), '--init-app', base['init_app']['source'],
               '--boot-image', str(args.base_run / 'limine-boot.img'),
               '--storage-boot', selected['/srv/storage_boot.elf'],
               '--seed0root', selected['/sbin/seed0root.elf'], '--filed', selected['/sbin/filed.elf'],
               '--package', str(package),
               '--timeout', str(args.timeout)]
    if args.publication_only:
        command += ['--sandbox-bench-only']
    else:
        command += ['--native-smoke', '--direct-smoke', '--multiflight-smoke']
        for entry in base['installs']:
            command += ['--install', entry['path'] + '=' + entry['source']]
    save(args.out / 'command.json', command)
    owned = ROOT / 'kobox2/linux-sandbox/kobox'
    sources = [Path(__file__)] + [owned / name for name in (
        'boot/fs_bench.c', 'boot/fs_bench.h', 'boot/fs_service.c',
        'boot/fs_service.h', 'boot/fs_worker.c', 'boot/fs_worker.h', 'boot/fs_executor.c', 'boot/fs_executor.h',
        'boot/fs_port.c', 'boot/fs_port_internal.h', 'boot/fs_mount.c',
        'boot/build_boot_runtime.py', 'boot/sources.py', 'task/port.c',
        'task/include/asm/preempt.h', 'task/irq_transaction.h', 'task/host.h', 'task/diagnostic.h',
        'arch/x86_64/host_call.h')]
    if args.adapter:
        sources += [ROOT / 'userland/kobox2_adapter' / name
                    for name in ('machine.c', 'owner_wait.h', 'fiber.S', 'build-sandbox.sh',
                                 'exception.c', 'irq_callbacks.h', 'native.c', 'boot.c', 'runtime.c', 'host.h',
                                 'image.c', 'entry.S', 'clock_callbacks.h')]
    snapshot = args.out / 'sources'
    snapshot.mkdir()
    for source in sources:
        destination = snapshot / source.relative_to(ROOT)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
    save(args.out / 'inputs.json', {'runtime_sha256': digest(args.runtime / 'linux-boot-runtime.so'),
        'kernel_source': str(kernel), 'kernel_sha256': digest(kernel),
        'adapter_source': str(adapter), 'adapter_sha256': digest(adapter),
        'runtime_inputs_sha256': digest(args.runtime / 'linux-boot-inputs.json'),
        'elapsed_only': args.elapsed_only,
        'handoff_only': args.handoff_only,
        'publication_only': args.publication_only,
        'executor_only': args.executor_only,
        'runtime_flags': flags,
        'base_inputs_sha256': digest(args.base_run / 'inputs.json'),
        'config': json.loads((args.runtime / 'linux-boot-inputs.json').read_text())['linux'],
        'sources': [{'path': str(p), 'sha256': digest(p)} for p in sources],
        'boundary': ('sandbox-private queue; worker submit/collect versus executor intake/recycle; worker-only control; full producer-to-drain/check wall time; no ring, IPC or LPR; depth8 is throughput'
                     if args.executor_only else
                     'sandbox-private sink; paired owner/worker publication timestamps; no ring, IPC or LPR; depth8 is throughput'
                     if args.publication_only else
                     'normal diagnostic-OFF integration verification; no speed measurement'
                     if args.verify_only else
                     'same-logical-CPU task roundtrip; no FS; detailed probes OFF'
                     if args.handoff_only else
                     'IPC-free in-sandbox; detailed probes OFF; workload checks included; depth8 is throughput'
                     if args.elapsed_only else
                     'IPC-free in-sandbox; profile instrumentation; depth8 is throughput, not latency')})
    try:
        subprocess.run(command, check=True)
        if not args.verify_only:
            collect(args.out, args.elapsed_only, args.handoff_only, args.publication_only, args.executor_only)
    finally:
        workload = args.out / 'qemu/pacha.qcow2'
        if workload.exists():
            before = digest(workload)
            checker = args.out / 'fsck.qcow2'
            overlay(checker, workload, 'qcow2')
            text = linux_script(args, checker, 'fsck', 'fsck.sh', 'FILED_NVME_FSCK_OK')
            if 'NVME_FSCK_FINAL_RC=0' not in text or digest(workload) != before:
                raise RuntimeError('offline fsck failed or workload disk changed')
            save(args.out / 'fsck.json', {'final_rc': 0, 'workload_unchanged': True,
                'before_rc': int(re.search(r'NVME_FSCK_BEFORE_REPLAY_RC=(\d+)', text)[1])})
    print('FS_SANDBOX_VERIFY_OK' if args.verify_only else 'FS_SANDBOX_BENCHMARK_OK',
          args.out, flush=True)


if __name__ == '__main__':
    main()
