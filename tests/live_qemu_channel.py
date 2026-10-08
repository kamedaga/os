#!/usr/bin/env python3
"""Use an idle harness's verified QEMU connection without restarting the VM.

Requires ptrace permission for pidfd_getfd. Console output stays with its
existing reader; QMP use requires that the harness is idle, not issuing RPCs.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import socket
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--controller', type=int, required=True)
p.add_argument('--qemu', type=int, required=True)
p.add_argument('--fd', type=int, required=True)
p.add_argument('--peer', required=True)
a = p.add_mutually_exclusive_group(required=True)
a.add_argument('--console')
a.add_argument('--qmp')
args = p.parse_args()
status = Path(f'/proc/{args.qemu}/status').read_text()
assert f'PPid:\t{args.controller}\n' in status
assert 'qemu-system-' in os.readlink(f'/proc/{args.qemu}/exe')
libc = ctypes.CDLL(None, use_errno=True)
pidfd = os.pidfd_open(args.controller)
try:
    fd = libc.syscall(438, pidfd, args.fd, 0)  # x86_64 pidfd_getfd
    if fd < 0: raise OSError(ctypes.get_errno(), 'pidfd_getfd')
finally:
    os.close(pidfd)
with socket.socket(fileno=fd) as channel:
    assert channel.getpeername() == args.peer
    if args.console is not None:
        assert '\n' not in args.console and '\r' not in args.console
        channel.sendall(args.console.encode() + b'\n')
        print('CONSOLE_SENT')
    else:
        request = json.loads(args.qmp)
        ident = f'live-diag-{os.getpid()}-{time.monotonic_ns()}'
        request['id'] = ident
        channel.settimeout(5)
        channel.sendall(json.dumps(request).encode() + b'\n')
        data = b''
        while True:
            data += channel.recv(65536)
            while b'\n' in data:
                line, data = data.split(b'\n', 1)
                reply = json.loads(line)
                if reply.get('id') == ident:
                    print(json.dumps(reply))
                    raise SystemExit(1 if 'error' in reply else 0)
