#!/usr/bin/env python3
"""Bounded, paused kernel-frame snapshots during one real navigation.

Diagnostic only: pauses perturb page timings. The exact installed ELF is
required. No user stack is read. Preserve pause duration and raw registers.
"""
import argparse
import bisect
import json
from pathlib import Path
import re
import subprocess
import time

from qemu_page_load_samples import NavigationGate
from qemu_xfce_apk_add_smoke import QMP


def register(text, name):
    return int(re.search(r'\b' + name + r'=([0-9a-f]+)', text)[1], 16)


def frame_in_stack(frame, rsp):
    return (0xffff800000000000 <= rsp <= frame < rsp + 131072 and frame % 8 == 0)


def raw_kernel_frames_allowed(registers):
    return ('CPL=0' in registers and 'HLT=1' not in registers and
            0xffff800000000000 <= register(registers, 'RIP') <= 0xffffffffffffffff)


def sample_interval(value):
    result = float(value)
    if not .1 <= result <= 5:
        raise argparse.ArgumentTypeError('interval must be between .1 and 5 seconds')
    return result


def user_code(registers, hmp):
    """Read only 32 instruction bytes, never a user stack or heap."""
    if not re.search(r'\bCPL=3\b', registers) or 'HLT=1' in registers:
        return None
    address = register(registers, 'RIP')
    raw = hmp(f'x /32bx 0x{address:x}')
    octets = re.findall(r'0x([0-9a-f]{2})(?![0-9a-f])', raw)
    return dict(address=address, hex=''.join(octets) if len(octets) == 32 else None, raw=raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qmp', 'elf', 'page-log', 'out'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--page-uri', required=True,
                        help="Initial URI to match, or '*' for the next navigation")
    parser.add_argument('--interval', type=sample_interval, default=1,
                        help='Seconds between snapshots; still capped at 25 snapshots')
    parser.add_argument('--post-load-seconds', type=float, default=0,
                        help='Continue up to 15 seconds after load for SPA attribution')
    parser.add_argument('--user-code', action='store_true',
                        help='Also capture 32 instruction bytes at user RIP for exact module matching')
    parser.add_argument('--raw-kernel-frames', action='store_true',
                        help='Retain bounded kernel frames without symbols for offline disassembly')
    args = parser.parse_args()
    if not 0 <= args.post_load_seconds <= 15:
        parser.error('--post-load-seconds must be between 0 and 15')
    out = Path(args.out)
    if out.exists():
        parser.error('choose a new output path')
    symbols = []
    for line in subprocess.check_output(['nm', '-S', '-n', args.elf], text=True).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[2] in ('t', 'T'):
            symbols.append((int(fields[0], 16), int(fields[1], 16), fields[3]))
    symbols.sort()
    starts = [s[0] for s in symbols]

    def symbol(address):
        i = bisect.bisect_right(starts, address) - 1
        if i >= 0 and address < symbols[i][0] + symbols[i][1]:
            return symbols[i][2]
        return None

    gate = NavigationGate(args.page_uri)
    snapshots = []
    qmp = None
    started = time.monotonic()
    navigation_start = None
    loaded = None
    reason = 'start_timeout'
    try:
        with Path(args.page_log).open(errors='replace') as log:
            log.seek(0, 2)
            print('Stack probe ready', flush=True)
            while True:
                gate.feed(log.read())
                if gate.complete:
                    if loaded is None:
                        loaded = time.monotonic()
                    if time.monotonic() - loaded >= args.post_load_seconds:
                        reason = 'post_load_complete' if args.post_load_seconds else 'load_event_3'
                        break
                now = time.monotonic()
                if gate.view is None:
                    if now - started > 45:
                        break
                    time.sleep(.1)
                    continue
                if navigation_start is None:
                    navigation_start = now
                    qmp = QMP(args.qmp)
                if now - navigation_start > 45 or len(snapshots) >= 25:
                    reason = 'snapshot_limit'
                    break
                snapshot = dict(epoch=time.time(), cpus=[])
                before = time.monotonic()
                try:
                    qmp.execute('stop')
                    if qmp.execute('query-status')['running']:
                        raise RuntimeError('VM has not stopped')
                    for cpu in qmp.execute('query-cpus-fast'):
                        index = cpu['cpu-index']
                        def hmp(command):
                            return qmp.execute('human-monitor-command',
                                {'command-line': command, 'cpu-index': index})
                        registers = hmp('info registers')
                        rip, rbp, rsp = [register(registers, n) for n in ('RIP', 'RBP', 'RSP')]
                        row = dict(cpu=index, registers=registers, frames=[], symbol=symbol(rip))
                        snapshot['cpus'].append(row)
                        if args.user_code:
                            row['user_code'] = user_code(registers, hmp)
                        raw_frames = args.raw_kernel_frames and raw_kernel_frames_allowed(registers)
                        if 'HLT=1' in registers or (symbol(rip) is None and not raw_frames):
                            continue
                        visited = set()
                        for _ in range(6):
                            if not frame_in_stack(rbp, rsp) or rbp in visited:
                                break
                            visited.add(rbp)
                            raw = hmp(f'x /2gx 0x{rbp:x}')
                            words = re.findall(r'0x([0-9a-f]{16})(?!:)', raw)
                            if len(words) != 2:
                                break
                            parent, ret = [int(w, 16) for w in words]
                            row['frames'].append(dict(rbp=rbp, parent=parent, ret=ret,
                                symbol=symbol(ret - 1), raw=raw))
                            if parent <= rbp or not 0xffff800000000000 <= ret <= 0xffffffffffffffff:
                                break
                            if symbol(ret - 1) is None and not raw_frames:
                                break
                            rbp = parent
                finally:
                    qmp.execute('cont')
                    snapshot['pause_seconds'] = time.monotonic() - before
                    snapshots.append(snapshot)
                time.sleep(args.interval)
    except Exception:
        reason = 'probe_error'
        raise
    finally:
        if qmp:
            qmp.close()
        out.write_text(json.dumps(dict(stop_reason=reason, snapshots=snapshots,
            navigation_events=gate.events,
            note='Paused diagnostic; not a normal-release speed comparison.')))
    print(json.dumps(dict(snapshots=len(snapshots), stop_reason=reason,
        pause_seconds=sum(s['pause_seconds'] for s in snapshots))))


if __name__ == '__main__':
    main()
