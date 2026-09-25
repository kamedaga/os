#!/usr/bin/env python3
"""Select exclusive main-thread bins strictly before observed first paint."""
import argparse
import json
from pathlib import Path
import re
from statistics import median

KINDS = ['read', 'readv', 'pread', 'open/openat', 'stat/fstat', 'lseek',
         'close', 'mmap(file)', 'mmap(anonymous)', 'munmap', 'mprotect',
         'write/writev', 'socket/connect', 'send', 'recv', 'ioctl',
         'poll/select/epoll', 'futex', 'process/wait', 'clock/sleep',
         'path/directory', 'signal', 'other VM', 'other syscall']
DETAILS = ['seek.pin', 'seek.local', 'seek.wire_page', 'seek.rpc',
           'seek.wire_release', 'seek.finish', 'seek.unpin', 'reserved',
           'seek.SET.calls', 'seek.CUR.calls', 'seek.END.calls',
           'poll.scan', 'poll.graph', 'poll.block', 'wait.native', 'wait.drain']
WAIT_TYPES = ['unidentified', 'socket', 'pipe', 'TTY', 'eventfd', 'timerfd',
              'inotify', 'signalfd', 'DRM', 'input', 'sync_file', 'epoll']


def analyze(directory):
    records = {}
    current = {}
    exec_rows = []
    pending_peer = None
    dumping = None
    lines = []
    for source in ['serial.log', 'console.log']:
        if not (directory/source).exists():
            continue
        for lineno, line in enumerate((directory/source).read_text().splitlines(), 1):
            lines.append((float(line.split(' ', 1)[0]), source, lineno, line))
    for _, source, lineno, line in sorted(lines, key=lambda item:item[0]):
        peer = re.search(r'\[gui-peer\] (.*)', line)
        if peer and pending_peer is not None:
            pending_peer['path'] = peer[1]
            pending_peer = None
        exe = re.search(r'^([\d.]+) \[gui-exec-cycles\] (\S+) (\S+) (\d+)', line)
        if exe:
            exec_rows.append((float(exe[1]), exe[2], exe[3], int(exe[4])))
        m = re.fullmatch(r'([\d.]+) \[gui-prof\] (\w+) ([\d ]+)', line)
        if not m:
            if '[gui-prof]' in line:
                damaged = re.search(r'\[gui-prof\] \w+ (\d+)', line)
                if damaged and int(damaged[1]) in current:
                    current[int(damaged[1])]['trace_error'] = f'malformed trace at {source}:{lineno}'
                elif dumping is not None:
                    dumping['trace_error'] = f'malformed trace during dump at {source}:{lineno}'
                else:
                    raise ValueError(f'unattributable damaged trace at {source}:{lineno}')
            continue
        host, tag = float(m[1]), m[2]
        v = list(map(int, m[3].split()))
        pid = v[0]
        lengths = dict(begin=(6,), end=(3,), done=(1,), bin=(5,), detail=(5,),
                       file=(9,), wait=(10, 11), peer=(4,), dropped=(3,))
        if tag not in lengths or len(v) not in lengths[tag]:
            if pid in current:
                current[pid]['trace_error'] = f'malformed {tag} at {source}:{lineno}'
                continue
            raise ValueError(f'malformed trace at {source}:{lineno}')
        if tag == 'begin':
            record = dict(pid=pid, tid=v[1], app=v[2], start_tsc=v[3],
                          start_ns=v[4], width=v[5], host=host, bins={}, details={},
                          waits=[], peers=[], dropped=[0, 0], done=False)
            key = (pid, v[3])
            records[key] = record
            current[pid] = record
        elif pid in current:
            record = current[pid]
            if tag == 'end':
                record.update(end_tsc=v[1], end_ns=v[2], end_host=host)
                dumping = record
            elif tag == 'done':
                record['done'] = True
                record['done_host'] = host
                dumping = None
            elif tag == 'bin':
                assert (v[1], v[2]) not in record['bins'], 'duplicate bin'
                record['bins'][v[1], v[2]] = v[3:5]
            elif tag == 'detail':
                record['details'][v[1], v[2]-24] = v[3:5]
            elif tag == 'wait':
                record['waits'].append(dict(zip(
                    ['start', 'end', 'fd', 'events', 'leaves', 'ready', 'timeout', 'status', 'handle', 'type'], v[1:])))
            elif tag == 'peer':
                pending_peer = dict(tsc=v[1], fd=v[2], handle=v[3], path='unknown')
                record['peers'].append(pending_peer)
            elif tag == 'dropped':
                record['dropped'] = v[1:3]

    # Short exit-to-start calibrations amplify serial reader scheduling delay.
    # Fit one invariant-TSC rate to long-baseline anchors from the whole run.
    anchors = [(r['host'], r['start_tsc']) for r in records.values()]
    anchors += [(r['end_host'], r['end_tsc']) for r in records.values() if 'end_host' in r]
    slopes = [(t2-t1)/(h2-h1) for h1,t1 in anchors for h2,t2 in anchors if h2-h1 >= 10]
    if not slopes:
        raise ValueError('need at least ten seconds of timestamp anchors')
    hz = median(slopes)
    offset = median(tsc-host*hz for host,tsc in anchors)
    max_residual = max(abs((tsc-offset)/hz-host) for host,tsc in anchors)
    output = []
    events = json.loads((directory/'results.json').read_text())['events']
    for event in events:
        name = event['event']
        app = 1 if name.startswith('gtk-') else 2 if name.startswith('terminal-') else 0
        if not app or 'start_host_s' not in event:
            continue
        if event.get('invalid_reason'):
            output.append(dict(event=name, error=event['invalid_reason']))
            continue
        matches = [r for r in records.values() if r['app'] == app and
                   event['start_host_s'] <= r['host'] <= event['paint_upper_host_s']]
        if len(matches) != 1:
            output.append(dict(event=name, error=f'{len(matches)} matching processes'))
            continue
        r = matches[0]
        overlapping = [previous['pid'] for previous in records.values()
                       if previous is not r and previous.get('end_host', float('inf')) <= event['start_host_s']
                       < previous.get('done_host', float('inf'))]
        if overlapping:
            output.append(dict(event=name, error=f'launch overlaps exit dump of PIDs {overlapping}'))
            continue
        if not r['done']:
            output.append(dict(event=name, error=f'PID {r["pid"]}: exit dump incomplete'))
            continue
        if 'trace_error' in r:
            output.append(dict(event=name, error=r['trace_error']))
            continue
        bin_s = r['width'] / hz
        profile_host = (r['start_tsc']-offset)/hz
        # Retain an explicit unclassified tail: exclude observed anchor
        # scatter plus one extra bin, in addition to the last-negative frame.
        n = int((event['paint_lower_host_s']-profile_host-max_residual-bin_s)/bin_s)
        assert 0 < n <= 4096, 'paint outside trace capacity'
        categories = []
        for k, label in enumerate(KINDS):
            cycles = sum(c for (b, kind), (c, count) in r['bins'].items() if b < n and kind == k)
            count = sum(count for (b, kind), (c, count) in r['bins'].items() if b < n and kind == k)
            categories.append(dict(kind=label, ms=cycles/hz*1000, count=count))
        # A failure means nested/main-thread ownership is wrong: never publish
        # a wall-time table obtained by adding overlapping syscall spans.
        for b in range(n):
            used = sum(c for (i, k), (c, count) in r['bins'].items() if i == b)
            assert used <= r['width'], f'overlapping syscall spans in bin {b}'
        total = (event['lower_s']+event['upper_s'])/2
        covered = n*bin_s
        prefix = profile_host-event['start_host_s']
        tail = total-prefix-covered
        outside = covered*1000-sum(c['ms'] for c in categories)
        timeline = []
        for second in range(int(covered)+1):
            lo, hi = int(second/bin_s), min(n, int((second+1)/bin_s))
            if hi <= lo:
                continue
            spans = []
            for k, label in enumerate(KINDS):
                cycles = sum(c for (b, kind), (c, count) in r['bins'].items()
                             if lo <= b < hi and kind == k)
                spans.append((label, cycles/hz*1000))
            spans.append(('outside syscalls', (hi-lo)*bin_s*1000-sum(c for _, c in spans)))
            timeline.append(dict(from_s=lo*bin_s, to_s=hi*bin_s,
                                 top=sorted(spans, key=lambda p:-p[1])[:4]))
        # These are native waits, including those made by recv/read handlers.
        # They are nested detail, not additional startup wall time. Attribute
        # only unambiguous single-ready-leaf returns; do not guess a timeout's
        # peer. Match the underlying socket handle plus connect timestamp,
        # rather than a descriptor number that may have been closed/reused.
        waits = []
        cutoff = r['start_tsc'] + n*r['width']
        for wait in r['waits']:
            if not r['start_tsc'] <= wait['start'] <= wait['end'] <= cutoff:
                continue
            peers = [p for p in r['peers'] if wait['handle'] != 0 and
                     p['handle'] == wait['handle'] and p['tsc'] <= wait['start']]
            kind = WAIT_TYPES[wait.get('type', 0)]
            path = max(peers, key=lambda p:p['tsc'])['path'] if peers else f'FD {wait["fd"]} ({kind})'
            if wait['ready'] != 1:
                path = 'multiple ready' if wait['ready'] > 1 else 'no ready leaf / interruption'
            waits.append(dict(**wait, path=path, ms=(wait['end']-wait['start'])/hz*1000))
        output.append(dict(event=name, pid=r['pid'], tsc_hz=hz, bin_ms=bin_s*1000,
                           clock_anchor_max_residual_ms=max_residual*1000,
                           guest_clock_elapsed_s=(r['end_ns']-r['start_ns'])/1e9,
                           host_clock_elapsed_s=r['end_host']-r['host'],
                           total_ms=total*1000, lower_ms=event['lower_s']*1000,
                           upper_ms=event['upper_s']*1000,
                           prefix_ms=prefix*1000, covered_ms=covered*1000,
                           outside_syscalls_ms=outside, paint_tail_ms=tail*1000,
                           categories=categories, timeline=timeline,
                           native_waits=waits, trace_dropped=r['dropped'],
                           detail_stages=[dict(kind=label,
                               ms=sum(c for (b,k),(c,count) in r['details'].items() if b<n and k==kind)/hz*1000,
                               count=sum(count for (b,k),(c,count) in r['details'].items() if b<n and k==kind))
                               for kind,label in enumerate(DETAILS)] if r['details'] else [],
                           exec_stages_ms={stage:cycles/hz*1000 for host, exe, stage, cycles in exec_rows
                               if event['start_host_s'] <= host <= r['host'] and
                               exe == ('gtk4-demo' if app == 1 else 'xfce4-terminal')}))
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    (args.directory/'startup-breakdown.json').write_text(json.dumps(result, indent=2)+'\n')
    for row in result:
        if 'error' in row:
            print(row)
            continue
        print(f'\n{row["event"]}: {row["lower_ms"]:.1f}–{row["upper_ms"]:.1f} ms, '
              f'PID={row["pid"]}, {row["tsc_hz"]/1e9:.6f} GHz')
        print('kind | ms | % total | calls')
        items = [dict(kind='launch→profile start',ms=row['prefix_ms'],count=0)]
        items += row['categories']
        items += [dict(kind='outside syscalls',ms=row['outside_syscalls_ms'],count=0),
                  dict(kind='last bin→first paint',ms=row['paint_tail_ms'],count=0)]
        for item in items:
            if item['ms'] >= .1:
                print(f'{item["kind"]} | {item["ms"]:.2f} | '
                      f'{item["ms"]/row["total_ms"]*100:.2f} | {item["count"]}')
        if row['detail_stages']:
            print('Nested detail (not additional wall time): kind | ms | calls')
            for item in row['detail_stages']:
                if item['count']:
                    print(f'{item["kind"]} | {item["ms"]:.2f} | {item["count"]}')
        if row['native_waits']:
            print('Native waits by sole ready peer (all handlers): peer | ms | calls')
            for path in sorted({w['path'] for w in row['native_waits']}):
                waits = [w for w in row['native_waits'] if w['path'] == path]
                print(f'{path} | {sum(w["ms"] for w in waits):.2f} | {len(waits)}')
            print(f'Trace records dropped (wait, peer): {row["trace_dropped"]}')
