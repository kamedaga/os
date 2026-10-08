#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Loopback-only delivery/collection for a non-grabbing live guest input trace."""
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from tempfile import NamedTemporaryFile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--feedback-binary', type=Path)
    parser.add_argument('--state-binary', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=19015)
    args = parser.parse_args()
    binary = args.binary.read_bytes()
    feedback = args.feedback_binary.read_bytes() if args.feedback_binary else None
    state = args.state_binary.read_bytes() if args.state_binary else None
    args.out.mkdir(parents=True, exist_ok=True)
    script = f'''#!/bin/sh
set -eu
url=http://10.0.2.2:{args.port}
work=$(mktemp -d /tmp/pointer-trace.XXXXXX)
echo 'TRACE_DOWNLOAD_BEGIN'
busybox timeout 10 busybox wget -O "$work/trace" "$url/trace.elf"
chmod 700 "$work/trace"
echo '15 seconds: move the pointer normally/quickly to reproduce the jump; pause briefly after a jump.'
DISPLAY=:0 busybox timeout 30 "$work/trace" 15 > "$work/trace.log" 2>&1 || echo TRACE_PROCESS_FAILED >> "$work/trace.log"
cat /var/log/Xorg.0.log >> "$work/trace.log" 2>/dev/null || true
echo "Trace retained at $work/trace.log"
busybox timeout 15 busybox wget -O- --post-file="$work/trace.log" "$url/result"
'''.encode()
    feedback_script = f'''#!/bin/sh
set -eu
url=http://10.0.2.2:{args.port}
work=$(mktemp -d /tmp/cursor-feedback.XXXXXX)
busybox timeout 10 busybox wget -O "$work/probe" "$url/feedback.elf"
chmod 700 "$work/probe"
echo 'Move continuously for 10 seconds: visible 3s, hidden 4s, visible 3s. Cursor restores automatically.'
DISPLAY=:0 busybox timeout 20 "$work/probe" > "$work/feedback.log" 2>&1 || echo FEEDBACK_PROCESS_FAILED >> "$work/feedback.log"
echo "Trace retained at $work/feedback.log"
busybox timeout 15 busybox wget -O- --post-file="$work/feedback.log" "$url/result"
'''.encode()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            content = {'/probe.sh': script, '/trace.elf': binary,
                       '/feedback.sh': feedback_script if feedback else None,
                       '/feedback.elf': feedback,
                       '/cursor-state.elf': state}.get(self.path)
            if content is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Length', str(len(content)))
            self.end_headers()
            self.wfile.write(content)

        def do_POST(self):
            length = int(self.headers.get('Content-Length', '0'))
            if self.path != '/result' or not 0 < length <= 8 * 1024 * 1024:
                self.send_error(400)
                return
            content = self.rfile.read(length)
            if len(content) != length:
                self.send_error(400)
                return
            # Keep each observation rather than overwriting earlier evidence.
            with NamedTemporaryFile(
                    prefix='pointer-', suffix='.log', dir=args.out, delete=False) as stream:
                stream.write(content)
                print('TRACE_RECEIVED', stream.name, flush=True)
            self.send_response(200)
            self.send_header('Content-Length', '15')
            self.end_headers()
            self.wfile.write(b'TRACE_RECEIVED\n')

    print(f'POINTER_TRACE_SERVER port={args.port}', flush=True)
    HTTPServer(('127.0.0.1', args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
