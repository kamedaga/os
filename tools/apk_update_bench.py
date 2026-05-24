#!/usr/bin/env python3
import argparse
import csv
import math
import re
import statistics
import subprocess
import sys
from pathlib import Path


COUNTER_KEYS = [
    "LinuxAbiServer.perf.syscall_category.io",
    "LinuxAbiServer.perf.syscall_category.fd",
    "LinuxAbiServer.perf.syscall_category.fs",
    "LinuxAbiServer.perf.syscall_category.net",
    "LinuxAbiServer.perf.syscall_category.proc",
    "LinuxAbiServer.perf.syscall_category.vm",
    "LinuxAbiServer.perf.syscall_category.time",
    "LinuxAbiServer.perf.syscall_category.signal",
    "LinuxAbiServer.perf.syscall_category.misc",
    "LinuxAbiServer.perf.syscall_category.stub_ok",
    "LinuxAbiServer.perf.syscall_category.stub_err",
    "LinuxAbiServer.perf.pipe.create_calls",
    "LinuxAbiServer.perf.pipe.create_busy",
    "LinuxAbiServer.perf.pipe.create_faults",
    "LinuxAbiServer.perf.pipe.dup_refs",
    "LinuxAbiServer.perf.pipe.close_calls",
    "LinuxAbiServer.perf.pipe.deferred_wakes",
    "LinuxAbiServer.perf.pipe.wake_flushes",
    "LinuxAbiServer.perf.pipe.wake_replies",
    "LinuxAbiServer.perf.pipe.read_calls",
    "LinuxAbiServer.perf.pipe.read_bytes",
    "LinuxAbiServer.perf.pipe.read_blocked",
    "LinuxAbiServer.perf.pipe.read_again",
    "LinuxAbiServer.perf.pipe.read_eof",
    "LinuxAbiServer.perf.pipe.read_faults",
    "LinuxAbiServer.perf.pipe.write_calls",
    "LinuxAbiServer.perf.pipe.write_bytes",
    "LinuxAbiServer.perf.pipe.write_again",
    "LinuxAbiServer.perf.pipe.write_faults",
    "LinuxAbiServer.perf.pipe.write_broken",
    "LinuxAbiServer.perf.pipe.epoll_wait_calls",
    "LinuxAbiServer.perf.pipe.epoll_ready",
    "LinuxAbiServer.perf.net.requests",
    "LinuxAbiServer.perf.net.wait_calls",
    "LinuxAbiServer.perf.net.wait_loops",
    "LinuxAbiServer.perf.net.wait_slow",
    "LinuxAbiServer.perf.net.wait_timeouts",
    "LinuxAbiServer.perf.net.wait_op.tcp_read.calls",
    "LinuxAbiServer.perf.net.wait_op.tcp_read.loops",
    "LinuxAbiServer.perf.net.wait_op.tcp_read.slow",
    "LinuxAbiServer.perf.net.wait_op.tcp_read_bulk.calls",
    "LinuxAbiServer.perf.net.wait_op.tcp_read_bulk.loops",
    "LinuxAbiServer.perf.net.wait_op.tcp_read_bulk.slow",
    "LinuxAbiServer.perf.net.wait_context.poll_prefetch_read.calls",
    "LinuxAbiServer.perf.net.wait_context.poll_prefetch_read.loops",
    "LinuxAbiServer.perf.net.wait_context.poll_prefetch_read.slow",
    "LinuxAbiServer.perf.net.wait_context.poll_prefetch_read.timeouts",
    "LinuxAbiServer.perf.net.wait_context.recvmsg_blocking_read.calls",
    "LinuxAbiServer.perf.net.wait_context.recvmsg_blocking_read.loops",
    "LinuxAbiServer.perf.net.wait_context.recvmsg_blocking_read.slow",
    "LinuxAbiServer.perf.net.wait_context.recvmsg_blocking_read.timeouts",
    "LinuxAbiServer.perf.net.wait_context.recvmsg_nowait_read.calls",
    "LinuxAbiServer.perf.net.wait_context.recvmsg_nowait_read.loops",
    "LinuxAbiServer.perf.net.wait_context.recvmsg_nowait_read.slow",
    "LinuxAbiServer.perf.net.wait_context.recvmsg_nowait_read.timeouts",
    "LinuxAbiServer.perf.net.wait_context.read_inline_blocking.calls",
    "LinuxAbiServer.perf.net.wait_context.read_inline_blocking.loops",
    "LinuxAbiServer.perf.net.wait_context.read_inline_blocking.slow",
    "LinuxAbiServer.perf.net.wait_context.read_inline_blocking.timeouts",
    "LinuxAbiServer.perf.net.wait_context.read_inline_nowait.calls",
    "LinuxAbiServer.perf.net.wait_context.read_inline_nowait.loops",
    "LinuxAbiServer.perf.net.wait_context.read_inline_nowait.slow",
    "LinuxAbiServer.perf.net.wait_context.read_inline_nowait.timeouts",
    "LinuxAbiServer.perf.net.wait_context.read_bulk_blocking.calls",
    "LinuxAbiServer.perf.net.wait_context.read_bulk_blocking.loops",
    "LinuxAbiServer.perf.net.wait_context.read_bulk_blocking.slow",
    "LinuxAbiServer.perf.net.wait_context.read_bulk_blocking.timeouts",
    "LinuxAbiServer.perf.net.wait_context.read_bulk_nowait.calls",
    "LinuxAbiServer.perf.net.wait_context.read_bulk_nowait.loops",
    "LinuxAbiServer.perf.net.wait_context.read_bulk_nowait.slow",
    "LinuxAbiServer.perf.net.wait_context.read_bulk_nowait.timeouts",
    "LinuxAbiServer.perf.net.tcp_prefetch_attempts",
    "LinuxAbiServer.perf.net.tcp_prefetch_ready_hits",
    "LinuxAbiServer.perf.net.tcp_prefetch_bytes",
    "LinuxAbiServer.perf.net.tcp_prefetch_consumed",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.0.calls",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.0.bytes",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.1_512.calls",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.1_512.bytes",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.513_1500.calls",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.513_1500.bytes",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.1501_4096.calls",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.1501_4096.bytes",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.4097_16384.calls",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.4097_16384.bytes",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.16385_plus.calls",
    "LinuxAbiServer.perf.net.tcp_read_request_bucket.16385_plus.bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.0.calls",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.0.bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.1_512.calls",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.1_512.bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.513_1500.calls",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.513_1500.bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.1501_4096.calls",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.1501_4096.bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.4097_16384.calls",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.4097_16384.bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.16385_plus.calls",
    "LinuxAbiServer.perf.net.tcp_read_return_bucket.16385_plus.bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_zero_calls",
    "LinuxAbiServer.perf.net.tcp_read_return_prefetch_calls",
    "LinuxAbiServer.perf.net.tcp_read_return_prefetch_bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_direct_calls",
    "LinuxAbiServer.perf.net.tcp_read_return_direct_bytes",
    "LinuxAbiServer.perf.net.tcp_read_return_bulk_calls",
    "LinuxAbiServer.perf.net.tcp_read_return_bulk_bytes",
    "LinuxAbiServer.perf.vfs.wait_loops",
    "LinuxAbiServer.perf.vfs.wait_slow",
    "KernelExecProfile.ipc.wait_event_syscalls",
    "KernelExecProfile.ipc.wait_event_blocks",
    "KernelExecProfile.ipc.device_interrupts",
    "KernelExecProfile.item.signal_endpoint.count",
    "KernelExecProfile.item.wait_event.count",
    "KernelExecProfile.item.abi_reserve_reply_target_pages.count",
    "KernelExecProfile.item.abi_unmap_reply_target_pages.count",
    "proc.net.tcp_pending_read_defers",
    "proc.net.tcp_pending_read_completions",
    "proc.net.tcp_pending_read_rx_seen",
    "proc.net.tcp_pending_read_rx_completed",
    "proc.net.tcp_pending_read_wait_cycles",
    "proc.net.tcp_pending_read_wait_max_cycles",
    "proc.net.tcp_pending_read_rx_to_complete_cycles",
    "proc.net.tcp_pending_read_rx_to_complete_max_cycles",
    "proc.net.tcp_read_would_block_blocking",
    "proc.net.tcp_read_would_block_nowait",
    "proc.net.tcp_read_bulk_would_block_blocking",
    "proc.net.tcp_read_bulk_would_block_nowait",
    "proc.net.tcp_pending_read_defers_read",
    "proc.net.tcp_pending_read_defers_bulk",
    "proc.net.tcp_pending_read_retry_would_block_read",
    "proc.net.tcp_pending_read_retry_would_block_bulk",
    "proc.net.net_service_active_poll_rounds",
    "proc.net.net_service_active_poll_hits",
    "proc.net.net_service_active_poll_misses",
]


def normalize_serial(text: str) -> str:
    return re.sub(r"\[Thread \d+\]\s*", "", text)


def parse_number(value: str) -> int:
    return int(value.replace(",", ""))


def parse_stdout(text: str) -> dict[str, float | str]:
    row: dict[str, float | str] = {}
    for key in ("boot_wait_s", "command_s"):
        match = re.search(rf"^{key}=([0-9]+(?:\.[0-9]+)?)$", text, re.MULTILINE)
        if match:
            row[key] = float(match.group(1))
    row["result"] = "pass" if "--- timing ---" in text else "fail"
    for match in re.finditer(r"^((?:tcp_(?:pending_read|read|read_bulk)_[A-Za-z0-9_]+)|net_service_active_poll_[A-Za-z0-9_]+)=([0-9,]+)$", text, re.MULTILINE):
        row[f"proc.net.{match.group(1)}"] = parse_number(match.group(2))
    return row


def parse_serial(text: str) -> dict[str, int]:
    clean = normalize_serial(text)
    counters: dict[str, int] = {}
    flat = " ".join(clean.split())
    for match in re.finditer(r"(LinuxAbiServer\.perf\.[A-Za-z0-9_.]+)=\s*([0-9,]+)", flat):
        counters[match.group(1)] = parse_number(match.group(2))
    for match in re.finditer(r"LinuxAbiServer\.perf\.syscall_category\s+([A-Za-z0-9_]+)=\s*([0-9,]+)", flat):
        counters[f"LinuxAbiServer.perf.syscall_category.{match.group(1)}"] = parse_number(match.group(2))
    for match in re.finditer(r"KernelExecProfile\.ipc\s+([A-Za-z0-9_]+)=\s*([0-9,]+)", flat):
        counters[f"KernelExecProfile.ipc.{match.group(1)}"] = parse_number(match.group(2))
    for match in re.finditer(r"KernelExecProfile\.item\s+([A-Za-z0-9_]+)\s+count=([0-9,]+)\s+", flat):
        counters[f"KernelExecProfile.item.{match.group(1)}.count"] = parse_number(match.group(2))
    for match in re.finditer(
        r"LinuxAbiServer\.perf\.net\.wait_op\s+([A-Za-z0-9_]+)\s*"
        r"calls=\s*([0-9,]+)\s*loops=\s*([0-9,]+)\s*slow=\s*([0-9,]+)\s*timeouts=\s*([0-9,]+)",
        flat,
    ):
        op = match.group(1)
        counters[f"LinuxAbiServer.perf.net.wait_op.{op}.calls"] = parse_number(match.group(2))
        counters[f"LinuxAbiServer.perf.net.wait_op.{op}.loops"] = parse_number(match.group(3))
        counters[f"LinuxAbiServer.perf.net.wait_op.{op}.slow"] = parse_number(match.group(4))
        counters[f"LinuxAbiServer.perf.net.wait_op.{op}.timeouts"] = parse_number(match.group(5))
    for match in re.finditer(
        r"LinuxAbiServer\.perf\.net\.wait_context\s+([A-Za-z0-9_]+)\s*"
        r"calls=\s*([0-9,]+)\s*loops=\s*([0-9,]+)\s*slow=\s*([0-9,]+)\s*timeouts=\s*([0-9,]+)",
        flat,
    ):
        context = match.group(1)
        counters[f"LinuxAbiServer.perf.net.wait_context.{context}.calls"] = parse_number(match.group(2))
        counters[f"LinuxAbiServer.perf.net.wait_context.{context}.loops"] = parse_number(match.group(3))
        counters[f"LinuxAbiServer.perf.net.wait_context.{context}.slow"] = parse_number(match.group(4))
        counters[f"LinuxAbiServer.perf.net.wait_context.{context}.timeouts"] = parse_number(match.group(5))
    for match in re.finditer(
        r"LinuxAbiServer\.perf\.net\.(tcp_read_(?:request|return)_bucket)\s+([A-Za-z0-9_]+)\s*"
        r"calls=\s*([0-9,]+)\s*bytes=\s*([0-9,]+)",
        flat,
    ):
        prefix = match.group(1)
        bucket = match.group(2)
        counters[f"LinuxAbiServer.perf.net.{prefix}.{bucket}.calls"] = parse_number(match.group(3))
        counters[f"LinuxAbiServer.perf.net.{prefix}.{bucket}.bytes"] = parse_number(match.group(4))
    for line in clean.splitlines():
        line = " ".join(line.split())
        if not line:
            continue
        match = re.match(r"(LinuxAbiServer\.perf\.[A-Za-z0-9_.]+)=([0-9,]+)$", line)
        if match:
            counters[match.group(1)] = parse_number(match.group(2))
            continue
        match = re.match(r"LinuxAbiServer\.perf\.syscall_category ([A-Za-z0-9_]+)=([0-9,]+)$", line)
        if match:
            counters[f"LinuxAbiServer.perf.syscall_category.{match.group(1)}"] = parse_number(match.group(2))
            continue
        match = re.match(
            r"LinuxAbiServer\.perf\.net\.wait_op ([A-Za-z0-9_]+) "
            r"calls=\s*([0-9,]+) loops=\s*([0-9,]+) slow=\s*([0-9,]+) timeouts=\s*([0-9,]+)$",
            line,
        )
        if match:
            op = match.group(1)
            counters[f"LinuxAbiServer.perf.net.wait_op.{op}.calls"] = parse_number(match.group(2))
            counters[f"LinuxAbiServer.perf.net.wait_op.{op}.loops"] = parse_number(match.group(3))
            counters[f"LinuxAbiServer.perf.net.wait_op.{op}.slow"] = parse_number(match.group(4))
            counters[f"LinuxAbiServer.perf.net.wait_op.{op}.timeouts"] = parse_number(match.group(5))
            continue
        match = re.match(
            r"LinuxAbiServer\.perf\.net\.wait_context ([A-Za-z0-9_]+) "
            r"calls=\s*([0-9,]+) loops=\s*([0-9,]+) slow=\s*([0-9,]+) timeouts=\s*([0-9,]+)$",
            line,
        )
        if match:
            context = match.group(1)
            counters[f"LinuxAbiServer.perf.net.wait_context.{context}.calls"] = parse_number(match.group(2))
            counters[f"LinuxAbiServer.perf.net.wait_context.{context}.loops"] = parse_number(match.group(3))
            counters[f"LinuxAbiServer.perf.net.wait_context.{context}.slow"] = parse_number(match.group(4))
            counters[f"LinuxAbiServer.perf.net.wait_context.{context}.timeouts"] = parse_number(match.group(5))
            continue
        match = re.match(r"KernelExecProfile\.ipc\.([A-Za-z0-9_]+)=([0-9,]+)$", line)
        if match:
            counters[f"KernelExecProfile.ipc.{match.group(1)}"] = parse_number(match.group(2))
            continue
        match = re.match(r"KernelExecProfile\.item ([A-Za-z0-9_]+) count=([0-9,]+) ", line)
        if match:
            counters[f"KernelExecProfile.item.{match.group(1)}.count"] = parse_number(match.group(2))
    return counters


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    index = (len(ordered) - 1) * pct
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def pearson(xs: list[float], ys: list[float]) -> float:
    if len(xs) < 2 or len(xs) != len(ys):
        return math.nan
    mx = statistics.mean(xs)
    my = statistics.mean(ys)
    dx = [x - mx for x in xs]
    dy = [y - my for y in ys]
    denom = math.sqrt(sum(x * x for x in dx) * sum(y * y for y in dy))
    if denom == 0:
        return math.nan
    return sum(x * y for x, y in zip(dx, dy)) / denom


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    out = ["| " + " | ".join(headers) + " |"]
    out.append("| " + " | ".join("---" for _ in headers) + " |")
    for row in rows:
        out.append("| " + " | ".join(row) + " |")
    return "\n".join(out)


def write_summary(out_root: Path, rows: list[dict[str, object]], keys: list[str]) -> None:
    times = [float(row["command_s"]) for row in rows if isinstance(row.get("command_s"), float)]
    lines: list[str] = ["# apk update bench summary", ""]
    lines.append(f"- runs: {len(rows)}")
    if times:
        lines.extend(
            [
                f"- min command_s: {min(times):.3f}",
                f"- median command_s: {statistics.median(times):.3f}",
                f"- avg command_s: {statistics.mean(times):.3f}",
                f"- p90 command_s: {percentile(times, 0.90):.3f}",
                f"- p95 command_s: {percentile(times, 0.95):.3f}",
                f"- max command_s: {max(times):.3f}",
                "",
            ]
        )
    run_rows: list[list[str]] = []
    for row in rows:
        run_rows.append(
            [
                str(row["run"]),
                f"{float(row['command_s']):.3f}" if isinstance(row.get("command_s"), float) else "n/a",
                str(row.get("LinuxAbiServer.perf.net.wait_loops", "")),
                str(row.get("LinuxAbiServer.perf.net.wait_slow", "")),
                str(row.get("LinuxAbiServer.perf.net.wait_op.tcp_read.loops", "")),
                str(row.get("LinuxAbiServer.perf.net.wait_op.tcp_read_bulk.loops", "")),
                str(row.get("KernelExecProfile.ipc.wait_event_syscalls", "")),
                str(row.get("KernelExecProfile.ipc.device_interrupts", "")),
                str(row.get("proc.net.tcp_pending_read_defers", "")),
                str(row.get("proc.net.tcp_pending_read_wait_max_cycles", "")),
            ]
        )
    lines.append(markdown_table(
        [
            "Run",
            "command_s",
            "net wait loops",
            "net wait slow",
            "tcp_read loops",
            "tcp_read_bulk loops",
            "wait_event syscalls",
            "device interrupts",
            "pending defers",
            "pending max cycles",
        ],
        run_rows,
    ))
    lines.append("")

    category_names = ["io", "fd", "fs", "net", "proc", "vm", "time", "signal", "misc", "stub_ok", "stub_err"]
    category_rows: list[list[str]] = []
    for category in category_names:
        key = f"LinuxAbiServer.perf.syscall_category.{category}"
        values = [int(row.get(key, 0) or 0) for row in rows]
        if sum(values) == 0:
            continue
        category_rows.append([category, f"{statistics.mean(values):.1f}", str(max(values))])
    if category_rows:
        lines.extend(["", "## Syscall Categories", ""])
        lines.append(markdown_table(["Category", "Avg calls", "Max calls"], category_rows))
        lines.append("")

    pipe_rows: list[list[str]] = []
    for label, key in (
        ("create", "LinuxAbiServer.perf.pipe.create_calls"),
        ("create_busy", "LinuxAbiServer.perf.pipe.create_busy"),
        ("create_faults", "LinuxAbiServer.perf.pipe.create_faults"),
        ("dup_refs", "LinuxAbiServer.perf.pipe.dup_refs"),
        ("close", "LinuxAbiServer.perf.pipe.close_calls"),
        ("deferred_wakes", "LinuxAbiServer.perf.pipe.deferred_wakes"),
        ("wake_flushes", "LinuxAbiServer.perf.pipe.wake_flushes"),
        ("wake_replies", "LinuxAbiServer.perf.pipe.wake_replies"),
        ("read_calls", "LinuxAbiServer.perf.pipe.read_calls"),
        ("read_bytes", "LinuxAbiServer.perf.pipe.read_bytes"),
        ("read_blocked", "LinuxAbiServer.perf.pipe.read_blocked"),
        ("read_again", "LinuxAbiServer.perf.pipe.read_again"),
        ("read_eof", "LinuxAbiServer.perf.pipe.read_eof"),
        ("read_faults", "LinuxAbiServer.perf.pipe.read_faults"),
        ("write_calls", "LinuxAbiServer.perf.pipe.write_calls"),
        ("write_bytes", "LinuxAbiServer.perf.pipe.write_bytes"),
        ("write_again", "LinuxAbiServer.perf.pipe.write_again"),
        ("write_faults", "LinuxAbiServer.perf.pipe.write_faults"),
        ("write_broken", "LinuxAbiServer.perf.pipe.write_broken"),
        ("epoll_wait", "LinuxAbiServer.perf.pipe.epoll_wait_calls"),
        ("epoll_ready", "LinuxAbiServer.perf.pipe.epoll_ready"),
    ):
        values = [int(row.get(key, 0) or 0) for row in rows]
        if sum(values) == 0:
            continue
        pipe_rows.append([label, f"{statistics.mean(values):.1f}", str(max(values))])
    if pipe_rows:
        lines.extend(["", "## Pipe Counters", ""])
        lines.append(markdown_table(["Counter", "Avg", "Max"], pipe_rows))
        lines.append("")

    corr_rows: list[list[str]] = []
    for key in keys:
        xs: list[float] = []
        ys: list[float] = []
        for row in rows:
            if isinstance(row.get("command_s"), float) and isinstance(row.get(key), int):
                xs.append(float(row["command_s"]))
                ys.append(float(row[key]))
        value = pearson(xs, ys)
        if not math.isnan(value):
            corr_rows.append([key, f"{value:.3f}"])
    corr_rows.sort(key=lambda item: abs(float(item[1])), reverse=True)
    if corr_rows:
        lines.extend(["", "## Correlation with command_s", ""])
        lines.append(markdown_table(["Counter", "Pearson r"], corr_rows))
        lines.append("")

    bucket_rows: list[list[str]] = []
    for bucket in ("0", "1_512", "513_1500", "1501_4096", "4097_16384", "16385_plus"):
        req_calls = [int(row.get(f"LinuxAbiServer.perf.net.tcp_read_request_bucket.{bucket}.calls", 0) or 0) for row in rows]
        req_bytes = [int(row.get(f"LinuxAbiServer.perf.net.tcp_read_request_bucket.{bucket}.bytes", 0) or 0) for row in rows]
        ret_calls = [int(row.get(f"LinuxAbiServer.perf.net.tcp_read_return_bucket.{bucket}.calls", 0) or 0) for row in rows]
        ret_bytes = [int(row.get(f"LinuxAbiServer.perf.net.tcp_read_return_bucket.{bucket}.bytes", 0) or 0) for row in rows]
        if sum(req_calls) == 0 and sum(ret_calls) == 0:
            continue
        bucket_rows.append([
            bucket,
            f"{statistics.mean(req_calls):.1f}",
            f"{statistics.mean(req_bytes):.1f}",
            f"{statistics.mean(ret_calls):.1f}",
            f"{statistics.mean(ret_bytes):.1f}",
        ])
    if bucket_rows:
        lines.extend(["", "## TCP Read Size Buckets", ""])
        lines.append(markdown_table(["Bucket", "Avg req calls", "Avg req bytes", "Avg ret calls", "Avg ret bytes"], bucket_rows))
        lines.append("")
        source_rows = []
        for label, call_key, byte_key in (
            ("prefetch", "LinuxAbiServer.perf.net.tcp_read_return_prefetch_calls", "LinuxAbiServer.perf.net.tcp_read_return_prefetch_bytes"),
            ("direct", "LinuxAbiServer.perf.net.tcp_read_return_direct_calls", "LinuxAbiServer.perf.net.tcp_read_return_direct_bytes"),
            ("bulk", "LinuxAbiServer.perf.net.tcp_read_return_bulk_calls", "LinuxAbiServer.perf.net.tcp_read_return_bulk_bytes"),
        ):
            calls = [int(row.get(call_key, 0) or 0) for row in rows]
            bytes_ = [int(row.get(byte_key, 0) or 0) for row in rows]
            source_rows.append([label, f"{statistics.mean(calls):.1f}", f"{statistics.mean(bytes_):.1f}"])
        lines.append(markdown_table(["Source", "Avg calls", "Avg bytes"], source_rows))
        lines.append("")

    wait_context_rows = []
    for context in (
        "poll_prefetch_read",
        "recvmsg_blocking_read",
        "recvmsg_nowait_read",
        "read_inline_blocking",
        "read_inline_nowait",
        "read_bulk_blocking",
        "read_bulk_nowait",
    ):
        calls = [int(row.get(f"LinuxAbiServer.perf.net.wait_context.{context}.calls", 0) or 0) for row in rows]
        loops = [int(row.get(f"LinuxAbiServer.perf.net.wait_context.{context}.loops", 0) or 0) for row in rows]
        slow = [int(row.get(f"LinuxAbiServer.perf.net.wait_context.{context}.slow", 0) or 0) for row in rows]
        if sum(calls) == 0:
            continue
        wait_context_rows.append([
            context,
            f"{statistics.mean(calls):.1f}",
            f"{statistics.mean(loops):.1f}",
            f"{statistics.mean(slow):.1f}",
            str(max(loops)),
        ])
    if wait_context_rows:
        lines.extend(["", "## Net Wait Contexts", ""])
        lines.append(markdown_table(["Context", "Avg calls", "Avg loops", "Avg slow", "Max loops"], wait_context_rows))
        lines.append("")

    would_block_rows = []
    for label, key in (
        ("read blocking", "proc.net.tcp_read_would_block_blocking"),
        ("read nowait", "proc.net.tcp_read_would_block_nowait"),
        ("bulk blocking", "proc.net.tcp_read_bulk_would_block_blocking"),
        ("bulk nowait", "proc.net.tcp_read_bulk_would_block_nowait"),
        ("defer read", "proc.net.tcp_pending_read_defers_read"),
        ("defer bulk", "proc.net.tcp_pending_read_defers_bulk"),
        ("pending retry read would-block", "proc.net.tcp_pending_read_retry_would_block_read"),
        ("pending retry bulk would-block", "proc.net.tcp_pending_read_retry_would_block_bulk"),
    ):
        values = [int(row.get(key, 0) or 0) for row in rows]
        if sum(values) != 0:
            would_block_rows.append([label, f"{statistics.mean(values):.1f}", str(max(values))])
    if would_block_rows:
        lines.extend(["", "## TCP Would-Block Breakdown", ""])
        lines.append(markdown_table(["Counter", "Avg", "Max"], would_block_rows))
        lines.append("")

    out_root.joinpath("summary.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run repeated CapabilityOS apk update measurements and summarize counters.")
    parser.add_argument("--label", required=True, help="label used under .artifacts/apk-update-bench-<label>")
    parser.add_argument("--runs", type=int, default=20)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument(
        "--mode",
        choices=("plain", "profile", "detail"),
        default="detail",
        help="plain apk update, CAPABILITYOS_EXEC_PROFILE=1, or profile detail",
    )
    parser.add_argument("--command", default=None, help="override command run inside CapabilityOS")
    args = parser.parse_args()

    root = Path.cwd()
    out_root = root / ".artifacts" / f"apk-update-bench-{args.label}"
    out_root.mkdir(parents=True, exist_ok=True)

    command = args.command
    if command is None:
        if args.mode == "plain":
            command = "apk update"
        elif args.mode == "profile":
            command = "env CAPABILITYOS_EXEC_PROFILE=1 apk update"
        else:
            command = "env CAPABILITYOS_EXEC_PROFILE=1 CAPABILITYOS_EXEC_PROFILE_DETAIL=1 apk update"

    rows: list[dict[str, object]] = []
    for run in range(1, args.runs + 1):
        run_name = f"run{run:02d}"
        run_out = out_root / run_name
        cmd = [
            sys.executable,
            "tools/apk_update_smoke.py",
            "--out",
            str(run_out.relative_to(root)),
            "--timeout",
            str(args.timeout),
            "--command",
            command,
        ]
        print(f"{run_name}: {' '.join(cmd)}", flush=True)
        proc = subprocess.run(cmd, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        run_out.mkdir(parents=True, exist_ok=True)
        run_out.joinpath("driver.log").write_text(proc.stdout, encoding="utf-8", errors="replace")
        row: dict[str, object] = {"run": run, "out": str(run_out.relative_to(root)), "returncode": proc.returncode}
        row.update(parse_stdout(proc.stdout))
        serial_path = run_out / "serial.log"
        if serial_path.exists():
            row.update(parse_serial(serial_path.read_text(encoding="utf-8", errors="replace")))
        if proc.returncode != 0:
            row["result"] = "fail"
        rows.append(row)
        command_s = row.get("command_s")
        result = row.get("result", "fail")
        print(f"{run_name}: {result} command_s={command_s if command_s is not None else 'n/a'}", flush=True)

    fieldnames = ["run", "out", "result", "returncode", "boot_wait_s", "command_s"] + COUNTER_KEYS
    with out_root.joinpath("runs.tsv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    write_summary(out_root, rows, COUNTER_KEYS)
    print(f"wrote {out_root / 'runs.tsv'}")
    print(f"wrote {out_root / 'summary.md'}")
    return 1 if any(row.get("result") != "pass" for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
