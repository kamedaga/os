#!/usr/bin/env python3
"""DRM spans and same-thread read gaps. No summing of concurrent work."""
import collections
import json
from pathlib import Path
import statistics
import struct
import sys

def load(filename):
    rows = [r[:9] for r in struct.iter_unpack('<9QII', Path(filename).read_bytes()) if r[9]]
    assert rows[-1][3] == ord('Z') and rows[-1][4:6] == (len(rows)-1, 0)
    return sorted(rows[:-1])

server, app = map(load, sys.argv[1:3])
sent = collections.Counter()
for r in app:
    if r[3] == ord('W'):
        sent[r[4]] += r[8]
sender = sent.most_common(1)[0][0]
assert any(r[3] == ord('C') and r[4] == sender for r in server)
drm = [r for r in server if r[3] == ord('D')]
assert drm
groups = collections.defaultdict(list)
for r in drm:
    assert 80_000_000_000 <= r[6] <= r[0] < 90_000_000_000
    groups[r[4]].append(r)
commands = {}
for command, rows in groups.items():
    values = [(r[0]-r[6])/1e6 for r in rows]
    commands[hex(command)] = dict(count=len(rows), total_ms=sum(values),
        mean_ms=statistics.mean(values), p95_ms=sorted(values)[int((len(values)-1)*.95)],
        results=dict(collections.Counter(str(r[7] if r[7]<2**63 else r[7]-2**64) for r in rows)))

last_consume = {}
gaps = []
for r in server:
    if r[4] != sender or r[3] not in (ord('R'), ord('C')):
        continue
    if r[3] == ord('C'):
        last_consume[r[2]] = r[0]
    else:
        start = last_consume.pop(r[2], None)
        if start is not None and 80_000_000_000 <= start < r[0] < 90_000_000_000:
            gaps.append((start, r[0], r[2]))
assert gaps
gap_ns = sum(end-start for start,end,tid in gaps)
covered = 0
for start,end,tid in gaps:
    spans = sorted((max(start,r[6]), min(end,r[0])) for r in drm
                   if r[2] == tid and r[6] < end and r[0] > start)
    previous = start
    for a,b in spans:
        assert a >= previous, 'nested/overlapping DRM spans must not be summed'
        covered += b-a
        previous = b

# This counts repetition, not proof that queries are safe to cache. Other
# clients and asynchronous producers can change BO state without this thread.
seen = collections.defaultdict(set)
duplicates = 0
waits = [r for r in drm if r[4] == 0xc0086448]
for r in drm:
    key = (r[2],r[8])
    if r[4] != 0xc0086448:
        seen[key].clear()
    elif not r[7]:
        handle = r[5] & 0xffffffff
        duplicates += handle in seen[key]
        seen[key].add(handle)
print(json.dumps(dict(sender=sender, server_pid=server[0][1], commands=commands,
    read_gaps=dict(count=len(gaps), total_ms=gap_ns/1e6, drm_ms=covered/1e6,
                   other_ms=(gap_ns-covered)/1e6, drm_fraction=covered/gap_ns),
    wait=dict(count=len(waits), distinct_handles=len(set(r[5]&0xffffffff for r in waits)),
              flags=dict(collections.Counter(r[5]>>32 for r in waits)),
              repeated_success_without_other_ioctl=duplicates)), indent=2))
