#!/usr/bin/env python3
"""Disjoint wall-time phases of an application's frame clock, not GPU timings."""
import collections
import bisect
import argparse
import csv
import json
from pathlib import Path
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--start-us', type=int, help='Only fully contained frame intervals')
parser.add_argument('--end-us', type=int, help='Same guest monotonic clock as the observer')
parser.add_argument('files', nargs='+')
args = parser.parse_args()

names = ('before_update', 'update', 'layout', 'paint', 'after_paint',
         'between_frames', 'total')
for filename in args.files:
    with Path(filename).open() as source:
        metadata = source.readline().strip()
        assert 'dropped=0' in metadata, metadata
        lines = source.readlines()
        io_rows = []
        io_dropped = 0
        declared_io = None
        for line in lines:
            if line.startswith('# io_count='):
                declared_io = int(line.split('io_count=')[1].split()[0])
                io_dropped = int(line.split('io_dropped=')[1])
            if line.startswith('# io,'):
                _, op, fd, start, end, returned, *flags = line.strip().split(',')
                if op in ('poll', 'ppoll') and flags:
                    unit = 'timeout_ns' if op == 'ppoll' else 'timeout'
                    op += f' events={flags[0]} revents={flags[1]}'
                    if len(flags) >= 4:
                        op += f' {unit}={flags[2]} nfds={flags[3]}'
                io_rows.append((int(start), int(end), op, int(fd), int(returned)))
        if declared_io is not None:
            assert len(io_rows) == declared_io, \
                f'truncated I/O dump: received {len(io_rows)} of {declared_io} rows'
        if io_dropped:
            # The recorder is append-only on one GTK thread. An explicit
            # window ending before the last retained call is a complete
            # prefix, not permission to silently use a truncated full run.
            assert io_rows and args.end_us is not None and args.end_us <= io_rows[-1][0], \
                'I/O overflow: choose --end-us inside the retained prefix'
            assert all(a[1] <= b[0] for a, b in zip(io_rows, io_rows[1:]))
        clocks = collections.defaultdict(dict)
        for row in csv.DictReader(line for line in lines if not line.startswith('#')):
            frame = clocks[int(row['clock'])].setdefault(int(row['frame']), {})
            assert row['phase'] not in frame, row
            frame[row['phase']] = int(row['monotonic_us'])
    result = {}
    for clock, frames in clocks.items():
        samples = []
        paints = []
        between = []
        for number, frame in sorted(frames.items()):
            following = frames.get(number + 1, {})
            required = ('before', 'update', 'paint', 'after_begin', 'after_end')
            if not all(key in frame for key in required) or 'before' not in following:
                continue
            times = [frame['before'], frame['update'],
                     frame.get('layout', frame['paint']), frame['paint'],
                     frame['after_begin'], frame['after_end'], following['before']]
            assert times == sorted(times), (clock, number, times)
            if args.start_us is not None and times[0] < args.start_us:
                continue
            if args.end_us is not None and times[-1] > args.end_us:
                continue
            spans = [b-a for a, b in zip(times, times[1:])]
            samples.append(spans + [sum(spans)])
            paints.append((frame['paint'], frame['after_begin']))
            between.append((frame['after_end'], following['before']))
        if not samples:
            continue
        result[clock] = dict(frames=len(samples), phases_ms={
            name: dict(mean=statistics.mean(values)/1000,
                       median=statistics.median(values)/1000,
                       p95=sorted(values)[int((len(values)-1)*.95)]/1000)
            for name, values in zip(names, zip(*samples))})
        for label, windows in (('paint', paints), ('between_frames', between)):
            if not io_rows:
                continue
            starts = [a for a, b in windows]
            operations = collections.defaultdict(list)
            for start, end, op, fd, returned in io_rows:
                index = bisect.bisect_right(starts, start)-1
                if index >= 0 and end <= windows[index][1]:
                    operations[(op, fd)].append(end-start)
            result[clock][label + '_io'] = {
                f'{op} fd={fd}': dict(calls=len(values),
                    calls_per_frame=len(values)/len(samples),
                    mean_call_us=statistics.mean(values),
                    ms_per_frame=sum(values)/len(samples)/1000)
                for (op, fd), values in operations.items()}
            io_us = sum(sum(values) for values in operations.values())
            remaining = sum(b-a for a,b in windows)-io_us
            assert remaining >= 0, 'overlapping I/O spans'
            result[clock][label + '_outside_observed_io_ms'] = remaining/len(samples)/1000
    print(json.dumps(dict(file=filename, metadata=metadata,
        window_us=dict(start=args.start_us, end=args.end_us),
        io_dropped_outside_window=io_dropped, clocks=result), indent=2))
