#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


TIMED_LINE_RE = re.compile(r"^\[\+\s*([0-9]+(?:\.[0-9]+)?) ms\] (.*)$")
ANSI_RE = re.compile(r"\x1b(?:\[[0-9;?]*[ -/]*[@-~]|[@-_])")

MILESTONES = [
    ("BdsDxe loading", (r"BdsDxe: loading Boot0001",)),
    ("BdsDxe starting", (r"BdsDxe: starting Boot0001",)),
    ("RAW ENTER MAIN", (r"^RAW ENTER MAIN$",)),
    ("ENTER MAIN", (r"^ENTER MAIN$",)),
    ("collectMemoryStats begin", (r"\[stage\] collectMemoryStats begin",)),
    ("collectMemoryStats got map", (r"\[stage\] collectMemoryStats got map",)),
    ("USER_PAGE_READY", (r"^USER_PAGE_READY$",)),
    ("boot manifest ok", (r"boot manifest ok",)),
    ("seed exec ready", (r"seed exec ready",)),
    ("manager bootstrap table ready", (r"manager bootstrap table ready",)),
    ("spawn_exec seed", (r"spawn_exec ready child=1 thread=1",)),
    ("seed spawn returned", (r"seed spawn returned",)),
    ("manager grants begin", (r"manager grants begin",)),
    ("manager grants ready", (r"manager grants ready",)),
    ("seed slot", (r"seed slot=1",)),
    ("VirtioBlk started", (r"VirtioBlk: started",)),
    ("VirtioBlk queue ready", (r"VirtioBlk: queue ready",)),
    ("VirtioBlk connect request", (r"VirtioBlk: connect request",)),
    ("fat_server endpoint ready", (r"FatServer: endpoint ready",)),
    ("RootVfs endpoint ready", (r"RootVfs: endpoint ready",)),
    ("ExecService endpoint ready", (r"ExecService: endpoint ready",)),
    ("LinuxAbiServer started", (r"LinuxAbiServer: started",)),
    ("AP Linux child CPU1", (r"process_builder start child=.*sched_ap_place=1 assigned_cpu=1",)),
    ("AP Linux child CPU2", (r"process_builder start child=.*sched_ap_place=2 assigned_cpu=2",)),
    ("AP Linux child CPU3", (r"process_builder start child=.*sched_ap_place=3 assigned_cpu=3",)),
    ("dash basic-ok", (r"basic-ok",)),
    ("busybox-cat-ok", (r"busybox-cat-ok",)),
    ("busybox-fat-cat-ok", (r"busybox-fat-cat-ok",)),
    ("busybox-true-ok", (r"busybox-true-ok",)),
    ("busybox-false-ok", (r"busybox-false-ok",)),
    ("musl futex ok", (r"musl_smoke: futex ok",)),
    ("musl pthread ok", (r"musl_smoke: pthread ok",)),
    ("musl-smoke-ok", (r"musl-smoke-ok",)),
    ("pipe-ok", (r"pipe-ok",)),
    ("dash-smoke-done", (r"dash-smoke-done",)),
    ("LinuxAbiServer companion exit", (r"LinuxAbiServer: companion exit",)),
    ("ExecClient child done", (r"ExecClient: child done",)),
]

REQUIRED_MILESTONES = {
    "AP Linux child CPU1",
    "AP Linux child CPU2",
    "AP Linux child CPU3",
    "busybox-cat-ok",
    "busybox-fat-cat-ok",
    "busybox-true-ok",
    "busybox-false-ok",
    "musl futex ok",
    "musl-smoke-ok",
    "pipe-ok",
    "dash-smoke-done",
    "ExecClient child done",
}


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def parse_timed_lines(path: Path):
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = TIMED_LINE_RE.match(raw_line)
        if not match:
            continue
        elapsed_ms = float(match.group(1))
        text = strip_ansi(match.group(2))
        yield elapsed_ms, text


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", nargs="?", default=".artifacts/serial-timed.log")
    parser.add_argument(
        "--check",
        action="store_true",
        help="return non-zero when the AP Linux smoke milestones are incomplete",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    path = Path(args.path)
    if not path.is_file():
        print(f"missing timed log: {path}", file=sys.stderr)
        return 1

    found = {}
    for elapsed_ms, text in parse_timed_lines(path):
        for label, patterns in MILESTONES:
            if label in found:
                continue
            if any(re.search(pattern, text) for pattern in patterns):
                found[label] = (elapsed_ms, text)

    print(f"Boot timing summary from {path}")

    previous = None
    ordered_found = sorted(
        ((elapsed_ms, label, matched_text) for label, (elapsed_ms, matched_text) in found.items()),
        key=lambda item: item[0],
    )
    for elapsed_ms, label, matched_text in ordered_found:
        delta_ms = 0.0 if previous is None else elapsed_ms - previous
        print(f"{elapsed_ms:9.3f} ms  (+{delta_ms:8.3f} ms)  {label}")
        print(f"  {matched_text}")
        previous = elapsed_ms

    missing = [label for label, _patterns in MILESTONES if label not in found]
    if missing:
        print("missing milestones:")
        for label in missing:
            print(f"  {label}")

    missing_required = [
        label
        for label, _patterns in MILESTONES
        if label in REQUIRED_MILESTONES and label not in found
    ]
    if missing_required:
        print("missing required AP Linux smoke milestones:")
        for label in missing_required:
            print(f"  {label}")
    else:
        print("AP Linux smoke complete")

    end = found.get("ExecClient child done")
    start = found.get("BdsDxe loading")
    if start and end:
        total_ms = end[0] - start[0]
        print(f"total to ExecClient child done: {total_ms:.3f} ms")

    if args.check and missing_required:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
