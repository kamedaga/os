#!/usr/bin/env python3
"""Turn retained guest stage-bench console/serial evidence into Markdown."""
import argparse
import re
import statistics
from collections import defaultdict
from pathlib import Path

TYPES = {1: "STREAM", 2: "DGRAM", 5: "SEQPACKET"}
STAGES = ["total", "setup", "reserve", "copy", "commit", "notify", "watch", "wait",
          "cleanup", "route/head", "mapping", "serialize", "IPC_CALL", "reply wait",
          "validate", "reply close"]
OPS = "HELLO PROCESS_REGISTER PROCESS_CREDENTIALS THREAD_REGISTER SOCKET SOCKETPAIR BIND LISTEN CONNECT ACCEPT NAME PEERCRED POLL PENDING ATTACH RETAIN HANDOFF SHUTDOWN CLOSE FLAGS OPTIONS ERROR WAIT_REGISTER WAIT_SYNC WAIT_NOTIFY WAIT_REMOVE DGRAM_ROUTE DGRAM_COMMIT DGRAM_HEAD DGRAM_CONSUME RIGHTS_PREPARE RIGHTS_APPEND RIGHTS_COMMIT RIGHTS_CLAIM RIGHTS_FINISH RIGHTS_ACK RIGHTS_CANCEL DIAG".split()


def totals(directory):
    clocks, rows = {}, defaultdict(list)
    for line in (directory / "console-tty-test.log").read_text(errors="replace").splitlines():
        if not line.startswith(("UNIX_STAGE_CLOCK", "UNIX_STAGE_RESULT")):
            continue
        f = dict(re.findall(r"(\w+)=([^\s]+)", line))
        pid = int(f["pid"])
        if line.startswith("UNIX_STAGE_CLOCK"):
            clocks[pid] = int(f["ns"]) / int(f["ticks"])
        else:
            key = f["mode"], int(f["type"]), int(f["bytes"])
            rows[key].append(int(f["ticks"]) * clocks[pid] / int(f["iterations"]) / 1000)
    return rows


def compare(before, after):
    a, b = totals(before), totals(after)
    if not a or a.keys() != b.keys():
        raise ValueError("workload matrix mismatch")
    print("| mode | type | bytes | before µs | after µs | before/after |")
    print("| --- | --- | ---: | ---: | ---: | ---: |")
    for mode, kind, size in sorted(a):
        x = statistics.median(a[mode, kind, size])
        y = statistics.median(b[mode, kind, size])
        print(f"| {mode} | {TYPES[kind]} | {size} | {x:.3f} | {y:.3f} | {x / y:.3f} |")


def report(directory):
    console = (directory / "console-tty-test.log").read_text(errors="replace")
    serial = (directory / "serial-tty-test.log").read_text(errors="replace")
    clocks, workloads, runs, phases, done = {}, {}, defaultdict(list), defaultdict(list), set()
    for line in console.splitlines():
        if not line.startswith(("UNIX_STAGE_CLOCK ", "UNIX_STAGE_RESULT ", "UNIX_STAGE_PHASE ", "UNIX_STAGE_DONE ")):
            continue
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        pid = int(fields["pid"])
        if line.startswith("UNIX_STAGE_CLOCK"):
            clocks[pid] = int(fields["ns"]) / int(fields["ticks"])
        elif line.startswith("UNIX_STAGE_RESULT"):
            key = (fields["mode"], int(fields["type"]), int(fields["bytes"]))
            workloads[pid] = key
            runs[key].append(int(fields["ticks"]) * clocks[pid] / int(fields["iterations"]) / 1000)
        elif line.startswith("UNIX_STAGE_PHASE"):
            phases[(pid, fields["phase"])].append((int(fields["count"]), int(fields["ticks"])))
        elif line.startswith("UNIX_STAGE_DONE") and fields["status"] == "0":
            done.add(pid)
    if not workloads or set(workloads) != done:
        raise ValueError("missing workload or successful completion marker")
    print(f"# {directory.name}\n")
    print("TSC較正による経過µs。試行中央値。ready等は送信＋受信、rttは要求＋応答、named/abstractは接続・送受信・closeまで。\n")
    print("| mode | type | bytes | trials | µs/iteration median | min–max |")
    print("| --- | --- | ---: | ---: | ---: | ---: |")
    for (mode, kind, size), values in sorted(runs.items()):
        print(f"| {mode} | {TYPES[kind]} | {size} | {len(values)} | {statistics.median(values):.3f} | {min(values):.3f}–{max(values):.3f} |")
    print("\n## Linux呼出境界（試行の平均µs/callの中央値）\n")
    print("| workload | phase | µs/call |")
    print("| --- | --- | ---: |")
    for (pid, phase), values in sorted(phases.items()):
        key = workloads[pid]
        label = f"{key[0]}/{TYPES[key[1]]}/{key[2]}"
        value = statistics.median(t * clocks[pid] / c / 1000 for c, t in values)
        print(f"| {label} | {phase} | {value:.3f} |")
    records = {}
    for match in re.finditer(r"UNIX_PROFILE((?: [0-9a-f]{16}){6})", serial):
        pid, group, key, stage, count, ticks = [int(v, 16) for v in match[1].split()]
        if pid not in workloads:
            continue
        identity = pid, group, key, stage
        if identity in records:
            raise ValueError(f"duplicate profile row: {identity}")
        records[identity] = count, ticks
    if not records:
        return
    print("\n## LPR内訳（全試行合計。入れ子はinclusive）\n")
    print("| workload | group/key | stage | count | µs/call | total ms |")
    print("| --- | --- | --- | ---: | ---: | ---: |")
    for (pid, group, key, stage), (count, ticks) in sorted(records.items()):
        workload = workloads[pid]
        label = f"{workload[0]}/{TYPES[workload[1]]}/{workload[2]}"
        if group == 0:
            name = f"{TYPES[key // 2]}/{'send' if key % 2 else 'recv'}"
        elif group in (1, 2):
            name = f"{'RPC' if group == 1 else 'control'}/{OPS[key]}"
        else:
            name = "empty timer"
        time_ns = ticks * clocks[pid]
        print(f"| {label} | {name} | {STAGES[stage]} | {count} | {time_ns / count / 1000:.3f} | {time_ns / 1e6:.3f} |")
    server = defaultdict(lambda: [0, 0])
    for match in re.finditer(r"UNIX_SERVER_PROFILE pid=(\d+) op=(\d+) stage=(\d+) count=(\d+) ticks=(\d+)", serial):
        pid, op, stage, count, ticks = map(int, match.groups())
        if pid in workloads:
            row = server[pid, op, stage]
            row[0] += count
            row[1] += ticks
    if server:
        print("\n## unixd側（session終了時出力、同一PIDのthread別sessionを合算）\n")
        print("| workload | operation | stage | count | µs/call |")
        print("| --- | --- | --- | ---: | ---: |")
        server_stages = ["receive", "check/map/snapshot", "dispatch/writeback", "unmap", "IPC_REPLY", "close"]
        for (pid, op, stage), (count, ticks) in sorted(server.items()):
            w = workloads[pid]
            label = f"{w[0]}/{TYPES[w[1]]}/{w[2]}"
            print(f"| {label} | {OPS[op]} | {server_stages[stage]} | {count} | {ticks * clocks[pid] / count / 1000:.3f} |")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--compare", type=Path, help="Compare this baseline with the supplied after directory")
    args = parser.parse_args()
    if args.compare:
        compare(args.directory, args.compare)
    else:
        report(args.directory)
