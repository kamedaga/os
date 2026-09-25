#!/usr/bin/env python3
"""Summarize labelled real-page trials; never infer success from load alone."""
import argparse
import collections
import json
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('log')
args = parser.parse_args()
trials = []
trial = None
with open(args.log, errors='replace') as source:
    for line in source:
        if line.startswith('PAGE_FONT_TEST '):
            trial = dict(label=line.split()[1], navigations=[], failures=[], terminations=[],
                         font_match_counts=collections.Counter(), time_calls={}, cycle_calls={},
                         malformed_timings=0)
            trials.append(trial)
        if trial is None:
            continue
        if line.startswith('PAGE_FAILED '):
            trial['failures'].append(line.strip())
        if line.startswith('PAGE_TERMINATED '):
            trial['terminations'].append(line.strip())
        if line.startswith('PAGE_TIMING ') and ' data=' in line:
            try:
                data = json.loads(line.split(' data=', 1)[1])
            except json.JSONDecodeError:
                trial['malformed_timings'] += 1
                continue
            trial['navigations'].append(dict(url=data['url'], navigation=data['n'],
                                            paint=data.get('paint', [])))
        match = re.search(r'PAGE_CALL_SUM ms=\d+ pid=(\d+) call=FcFontSetMatch calls=(\d+)', line)
        if match:
            trial['font_match_counts'][match[1]] += int(match[2])
        clock = re.search(r'PAGE_TIME_SUM ms=\d+ pid=(\d+) calls=(\d+) cycles=(\d+) '
                          r'calibration_ms=(\d+) calibration_cycles=(\d+)', line)
        if clock:
            pid, calls, cycles, calibration_ms, calibration_cycles = map(int, clock.groups())
            if calibration_cycles:
                value = trial['time_calls'].setdefault(str(pid), dict(calls=0, elapsed_ms=0.0))
                value['calls'] += calls
                value['elapsed_ms'] += cycles * calibration_ms / calibration_cycles
        cycle = re.search(r'PAGE_CYCLE_SUM ms=\d+ pid=(\d+) call=(\w+) calls=(\d+) cycles=(\d+) '
                          r'calibration_ms=(\d+) calibration_cycles=(\d+)', line)
        if cycle:
            pid, name, calls, cycles, calibration_ms, calibration_cycles = cycle.groups()
            if int(calibration_cycles):
                value = trial['cycle_calls'].setdefault(pid, {}).setdefault(name, dict(calls=0, elapsed_ms=0.0))
                value['calls'] += int(calls)
                value['elapsed_ms'] += int(cycles) * int(calibration_ms) / int(calibration_cycles)
print(json.dumps(dict(note='Bounded counters are lower bounds; load is not visual verification.',
                     time_note='Calibrated time() elapsed includes scheduling, not CPU time.',
                     sampling_note='Cycle names ending in Sampled contain only one-in-256 per-thread samples, not full-call totals.',
                     trials=trials), indent=2))
