#!/usr/bin/env python3
"""Expose one QMP Unix socket to a local Windows input client, without a new protocol."""
import argparse
import json
from pathlib import Path
import select
import signal
import socket


def main():
    def stop(_signal, _frame):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qmp", type=Path)
    parser.add_argument("--ready", required=True, type=Path)
    args = parser.parse_args()
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(1)
        args.ready.write_text(json.dumps({"host": "127.0.0.1", "port": listener.getsockname()[1]}) + "\n")
        print(args.ready.read_text(), flush=True)
        try:
            while True:
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
                try:
                    with client, socket.socket(socket.AF_UNIX) as qmp:
                        qmp.connect(str(args.qmp))
                        client.settimeout(2)
                        qmp.settimeout(2)
                        while True:
                            readable, _, _ = select.select([client, qmp], [], [], 1)
                            closed = False
                            for source in readable:
                                data = source.recv(65536)
                                if not data:
                                    closed = True
                                    break
                                (qmp if source is client else client).sendall(data)
                            if closed:
                                break
                except OSError as error:
                    print("QMP connection closed:", error, flush=True)
        finally:
            args.ready.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
