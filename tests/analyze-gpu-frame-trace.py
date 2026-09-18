#!/usr/bin/env python3
"""Join native adapter timestamps by correlation; never add nested spans."""
import collections
import json
from pathlib import Path
import re
import statistics
import sys

root = Path(sys.argv[1])
log = (root / 'serial.log').read_text()
hz = json.loads((root / 'tsc-frequency.json').read_text())
ms = hz / 1000
records = collections.defaultdict(dict)
footers = {}
pattern = re.compile(r'^\[frame-(gpud|sandbox)\] ' +
                     r' '.join([r'(\d+)'] * 8) + r'\s*$')
for line in log.splitlines():
    match = pattern.fullmatch(line)
    if not match:
        continue
    layer = match[1]
    row = tuple(map(int, match.groups()[1:]))
    if row[4] == row[5] == row[6] == row[7]:
        footers[layer] = row[:2]
        continue
    assert row[4] <= row[5] <= row[6] <= row[7], row
    assert row[0] not in records[layer], row
    records[layer][row[0]] = row
for layer in ('gpud', 'sandbox'):
    assert footers[layer] == (len(records[layer]), 0), (layer, footers, len(records[layer]))
print('TSC Hz:', hz, 'rows:', {k: len(v) for k, v in records.items()})

gpud = sorted(records['gpud'].values(), key=lambda r: r[4])
groups = collections.defaultdict(list)
for row in gpud:
    groups[(row[1], row[2])].append(row)
print('handle ioctl count total_ms mean_ms max_ms')
for (handle, op), rows in sorted(groups.items()):
    durations = [(r[7] - r[4]) / ms for r in rows]
    print(handle, hex(op), len(rows), round(sum(durations), 3),
          round(statistics.mean(durations), 3), round(max(durations), 3))

# DIRTYFB marks scanout updates, not a guaranteed GTK frame boundary. Preserve
# that distinction and report the observed sequence rather than assume 1:1.
scanouts = [r for r in gpud if r[2] == 0xc01864b1]
print('scanout intervals (ms): total, gpud-local, handoff/prep, Linux-dispatch, completion, rpc-other, outside-ioctl')
for first, last in zip(scanouts, scanouts[1:]):
    rows = [r for r in gpud if first[4] <= r[4] < last[4]]
    if any(r[0] not in records['sandbox'] for r in rows if r[6] > r[5]):
        print('incomplete interval', first[0], last[0])
        continue
    total = last[4] - first[4]
    local = prep = execute = complete = other = 0
    for r in rows:
        local += r[5] - r[4] + r[7] - r[6]
        if r[6] == r[5]:
            continue
        s = records['sandbox'][r[0]]
        # IPC_SEND may return after gpud has already received the reply.
        # Clip to the waiting caller's interval instead of double-counting
        # that concurrent sender tail (which would produce negative gaps).
        spans = [max(0, min(b, r[6]) - max(a, r[5]))
                 for a, b in zip(s[4:7], s[5:8])]
        prep += spans[0]
        execute += spans[1]
        complete += spans[2]
        other += r[6] - r[5] - sum(spans)
    outside = total - sum(r[7] - r[4] for r in rows)
    print(first[0], last[0], 'calls', len(rows),
          [round(v / ms, 3) for v in (total, local, prep, execute, complete, other, outside)],
          dict(collections.Counter(hex(r[2]) for r in rows)))

names = {int(number): name for name, number in re.findall(
    r'^#define (PACHA_\w+_SYSCALL_\w+) (\d+)$',
    (Path(__file__).resolve().parents[1] / 'userland/libpacha/include/pacha/abi.h').read_text(), re.M)}
syscalls = [tuple(map(int, row)) for row in re.findall(
    r'^\[frame-syscall\] (\d+) (\d+) (\d+) (\d+)\s*$', log, re.M)]
if syscalls:
    print('native syscalls inside DRM dispatch: name count total_ms mean_us max_ms')
    for nr, count, cycles, maximum in sorted(syscalls, key=lambda r: -r[2]):
        print(names.get(nr, nr), count, round(cycles / ms, 3),
              round(cycles / ms * 1000 / count, 3), round(maximum / ms, 3))
