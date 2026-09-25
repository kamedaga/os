#!/usr/bin/env python3
"""Join blocked writers to consumption of the same shared Unix stream.

Times are observations after consumption, not exact scheduling timestamps.
Only match output waits starting with less than the transport's 64-byte
readiness threshold. Never classify a read wait from its output occupancy.
"""
import bisect
import collections
import json
from pathlib import Path
import re
import statistics
import struct
import sys

rows = []
footers = {}
for filename in sys.argv[1:]:
    path = Path(filename)
    if path.suffix == '.bin':
        records = [r[:9] for r in struct.iter_unpack('<9QII', path.read_bytes()) if r[9]]
    else:
        records = [tuple(int(v, 16) for v in line.split()[1:])
                   for line in path.read_text().splitlines() if line.startswith('[unix-flow] ')]
    for row in records:
        assert len(row) == 9, row
        if row[3] == ord('Z'):
            footers[row[1]] = row[4:6]
        else:
            rows.append(row)
counts = collections.Counter(r[1] for r in rows)
for pid, count in counts.items():
    assert footers.get(pid) == (count, 0), (pid, count, footers.get(pid))
assert rows

def available(r):
    used = ((r[6] & 0xffffffff)-(r[7] & 0xffffffff)) & 0xffffffff
    return 65536-used

consumed = collections.defaultdict(list)
pending = {}
pairs = []
nonwritable_wakes = collections.Counter()
for r in sorted(rows):
    time, pid, tid, op, sender, generation, published, cursor, detail = r
    key = (pid, tid, sender, generation)
    if op == ord('C') and available(r) >= 64:
        consumed[sender, generation].append(r)
    elif op == ord('B'):
        assert key not in pending, key
        pending[key] = r
    elif op == ord('E'):
        start = pending.pop(key, None)
        if start and start[8] & 4 and available(start) < 64 and not detail:
            if available(r) >= 64:
                pairs.append((start, r))
            else:
                nonwritable_wakes[pid] += 1

results = collections.defaultdict(list)
unmatched = collections.Counter()
for start, end in pairs:
    candidates = consumed[start[4], start[5]]
    index = bisect.bisect_left(candidates, (start[0],))
    if index == len(candidates) or candidates[index][0] > end[0]:
        unmatched[start[1]] += 1
        continue
    consume = candidates[index]
    results[start[1]].append([(end[0]-start[0])/1e6,
        (consume[0]-start[0])/1e6, (end[0]-consume[0])/1e6])

summary = {}
for pid, values in results.items():
    summary[pid] = dict(matched=len(values), unmatched=unmatched[pid], phases_ms={
        name: dict(mean=statistics.mean(v), median=statistics.median(v),
                   p95=sorted(v)[int((len(v)-1)*.95)])
        for name, v in zip(('blocked_total','before_consumption','after_consumption'), zip(*values))})
print(json.dumps(dict(process_rows=counts, writers=summary,
                     nonwritable_wakes=nonwritable_wakes), indent=2))
