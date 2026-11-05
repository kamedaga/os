#!/usr/bin/env python3
"""Receive raw UDP datagrams to stdout; flush each packet for freeze capture."""
import argparse
import socket
import sys
p = argparse.ArgumentParser()
p.add_argument("--bind", default="0.0.0.0")
p.add_argument("--port", type=int, default=5000)
a = p.parse_args()
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
s.bind((a.bind, a.port))
while True:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()
