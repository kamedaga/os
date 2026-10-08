#!/usr/bin/env python3
"""Median ns_per_op per (fs, phase) for one or two FS_BENCH logs."""

import re
import statistics
import sys
from collections import defaultdict

LINE = re.compile(r"FS_BENCH fs=(\S+) phase=(\S+) ops=\d+ total_ns=\d+ ns_per_op=(\d+)")


def load(path):
    runs = defaultdict(list)
    with open(path, errors="replace") as handle:
        for match in LINE.finditer(handle.read()):
            runs[(match.group(1), match.group(2))].append(int(match.group(3)))
    return {key: int(statistics.median(values)) for key, values in runs.items()}


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: fs_microbench_summary.py PACHA_LOG [LINUX_LOG]")
    pacha = load(sys.argv[1])
    linux = load(sys.argv[2]) if len(sys.argv) == 3 else {}
    print(f"{'fs':6} {'phase':16} {'pacha_ns':>12} {'linux_ns':>12} {'ratio':>8}")
    for key in sorted(pacha):
        ref = linux.get(key)
        ratio = f"{pacha[key] / ref:.1f}x" if ref else "-"
        print(f"{key[0]:6} {key[1]:16} {pacha[key]:12} {ref if ref else '-':>12} {ratio:>8}")


if __name__ == "__main__":
    main()
