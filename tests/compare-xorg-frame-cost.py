#!/usr/bin/env python3
"""Partition complete PAGE_FLIP-request intervals, not GPU/display latency.

Linux input is Xorg-filtered syscall ftrace; Pacha input is the native LPR
binary recorder. Only spans on the flip caller's thread are included. The
unobserved remainder includes CPU work, scheduling and other calls/waits.
"""
import collections
import json
from pathlib import Path
import re
import struct
import sys

FLIP = 0xc01864b0
NAMES = {0xc0086448: 'bo_status_wait', 0xc0406442: 'submit',
         0xc018643b: 'kms', 0xc01c64ae: 'kms', FLIP: 'kms',
         0xc00464af: 'kms', 0x40086409: 'resource',
         0xc0386444: 'resource', 0xc0106441: 'resource'}


def linux_spans(path):
    text = Path(path).read_text()
    counts = re.search(r'entries-in-buffer/entries-written: (\d+)/(\d+)', text)
    assert counts and counts[1] == counts[2], 'ftrace loss or missing header'
    pending, spans = {}, []
    events = 0
    pattern = re.compile(r'\S+-(\d+)\s+\[\d+\]\s+\S+\s+(\d+)\.(\d+): sys_ioctl(.*)')
    for line in text.splitlines():
        m = pattern.fullmatch(line.strip())
        if not m:
            continue
        events += 1
        tid = int(m[1])
        ns = int(m[2])*10**9 + int(m[3].ljust(9, '0'))
        if m[4].startswith('('):
            assert tid not in pending, 'nested or lost ioctl exit'
            command = int(re.search(r'cmd: ([0-9a-f]+)', m[4])[1], 16) & 0xffffffff
            pending[tid] = (ns, command)
        else:
            assert tid in pending, 'unmatched ioctl exit'
            start, command = pending.pop(tid)
            spans.append((start, ns, tid, command))
    assert not pending and events == int(counts[1]), 'unmatched/unparsed trace events'
    return spans


def pacha_spans(path):
    rows = list(struct.iter_unpack('<9QII', Path(path).read_bytes()))
    footer = rows.pop()
    assert footer[3] == ord('Z') and footer[4:6] == (len(rows), 0)
    assert all(r[9] == 1 for r in rows), 'uncommitted trace rows'
    return [(r[6], r[0], r[2], r[4]) for r in rows if r[3] == ord('D')]


def summarize(spans):
    flips = sorted(r for r in spans if r[3] == FLIP)
    assert len(flips) >= 2 and len({r[2] for r in flips}) == 1
    tid = flips[0][2]
    spans = sorted(r for r in spans if r[2] == tid)
    assert all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), 'overlapping calls'
    first, last = flips[0][0], flips[-1][0]
    times, calls = collections.Counter(), collections.Counter()
    for start, end, _, command in spans:
        assert start <= end
        name = NAMES.get(command, 'other_ioctl')
        times[name] += max(0, min(last, end)-max(first, start))
        if first <= start < last:
            calls[name] += 1
    total = last-first
    times['outside_observed_ioctl'] = total-sum(times.values())
    assert times['outside_observed_ioctl'] >= 0
    n = len(flips)-1
    return dict(intervals=n, total_ms=total/1e6, interval_ms=total/n/1e6,
                flip_requests_per_second=n*1e9/total,
                parts={name: dict(ms_per_interval=ns/n/1e6,
                                 percent=ns*100/total,
                                 calls_per_interval=calls[name]/n)
                       for name, ns in sorted(times.items())})


if __name__ == '__main__':
    print(json.dumps(dict(
        definition='wall time between successive PAGE_FLIP requests; not GPU execution time',
        linux=summarize(linux_spans(sys.argv[1])),
        pacha=summarize(pacha_spans(sys.argv[2]))), indent=2))
