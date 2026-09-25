#!/usr/bin/env python3
"""Bounded, read-only vCPU samples during a real browser navigation.

Does not pause the VM. Live register snapshots are not atomic; retain QMP
latency and correlate with page events instead of treating samples as a trace.
"""
import argparse
import json
from pathlib import Path
import re
import time

from qemu_xfce_apk_add_smoke import QMP

class NavigationGate:
    """Match a new navigation ('*' accepts any URI), then retain its view."""

    def __init__(self, uri):
        self.uri = uri
        self.view = None
        self.complete = False
        self.pending = ''
        self.events = []

    def feed(self, chunk):
        self.pending += chunk
        lines = self.pending.split('\n')
        self.pending = lines.pop()
        for line in lines:
            match = re.fullmatch(r'PAGE_EVENT ms=(\d+) view=(\S+) event=(\d+) uri=(.*)',
                                 line.rstrip('\r'))
            if not match or self.complete:
                continue
            guest_ms, view, event, uri = match.groups()
            if self.view is None:
                if event != '0' or (self.uri != '*' and uri != self.uri):
                    continue
                self.view = view
            if view != self.view:
                continue
            self.events.append(dict(guest_ms=int(guest_ms), event=int(event), uri=uri,
                                    observed_epoch=time.time()))
            if event == '3':
                self.complete = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qmp', required=True)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--seconds', type=float, default=25)
    parser.add_argument('--interval', type=float, default=.05)
    parser.add_argument('--page-log', type=Path,
                        help='Tail new page events; start this probe before navigation')
    parser.add_argument('--page-uri', help="Initial URI to match, or '*' for the next navigation")
    parser.add_argument('--wait-seconds', type=float, default=45)
    parser.add_argument('--post-load-seconds', type=float, default=0,
                        help='Continue bounded sampling after load for SPA work (requires page gate)')
    args = parser.parse_args()
    if not 0 < args.seconds <= 45 or not 0 < args.wait_seconds <= 45:
        parser.error('sampling and start-wait durations must be between 0 and 45 seconds')
    if not .01 <= args.interval <= 5:
        parser.error('interval must be between .01 and 5 seconds')
    if bool(args.page_log) != bool(args.page_uri):
        parser.error('--page-log and --page-uri must be supplied together')
    if not 0 <= args.post_load_seconds <= 30 or (args.post_load_seconds and not args.page_log):
        parser.error('--post-load-seconds must be 0..30 and requires --page-log/--page-uri')
    if args.out.exists():
        parser.error('choose a new output path')
    gate = NavigationGate(args.page_uri) if args.page_log else None
    log = args.page_log.open(errors='replace') if gate else None
    if log:
        log.seek(0, 2)
    samples = []
    qmp = None
    started = time.monotonic()
    start = None if gate else started
    epoch = time.time()
    reason = 'start_timeout'
    loaded = None
    try:
        print('Sampling probe ready', flush=True)
        while True:
            if gate:
                gate.feed(log.read())
                if start is None and gate.view is not None:
                    start = time.monotonic()
                    epoch = time.time()
                if gate.complete:
                    if loaded is None:
                        loaded = time.monotonic()
                    if not args.post_load_seconds or time.monotonic() - loaded >= args.post_load_seconds:
                        reason = 'post_load_complete' if args.post_load_seconds else 'load_event_3'
                        break
            if start is None:
                if time.monotonic() - started >= args.wait_seconds:
                    break
                time.sleep(args.interval)
                continue
            if time.monotonic() - start >= args.seconds:
                reason = 'sampling_timeout'
                break
            # This QMP socket accepts one client. Do not occupy it while
            # waiting for the UI driver to initiate the navigation.
            if qmp is None:
                qmp = QMP(args.qmp)
                cpus = qmp.execute('query-cpus-fast')
            cpu = cpus[len(samples) % len(cpus)]['cpu-index']
            before = time.monotonic()
            registers = qmp.execute('human-monitor-command',
                {'command-line': 'info registers', 'cpu-index': cpu})
            samples.append(dict(cpu=cpu, elapsed=before-start,
                                cost=time.monotonic()-before, registers=registers))
            time.sleep(args.interval)
    except Exception:
        reason = 'probe_error'
        raise
    finally:
        if qmp:
            qmp.close()
        if log:
            log.close()
        args.out.write_text(json.dumps(dict(epoch=epoch, samples=samples, stop_reason=reason,
            load_finished_elapsed=loaded-start if loaded is not None and start is not None else None,
            post_load_seconds=args.post_load_seconds,
            navigation_events=gate.events if gate else [],
            note='Event 3 includes failures. Check page errors and visual rendering.')))
    print(json.dumps(dict(samples=len(samples), stop_reason=reason,
        duration=time.monotonic()-started,
        max_qmp_cost=max((s['cost'] for s in samples), default=0))))


if __name__ == '__main__':
    main()
