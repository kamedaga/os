#!/usr/bin/env python3
"""Decode the opt-in LPR/FileD startup profile from a QEMU serial log."""

from __future__ import annotations

import argparse
import os
import re
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1
TRACE_RE = re.compile(r"\[trace\] c=(\S+) e=(\S+).*?((?: a\d+=\d+)+)")
ARG_RE = re.compile(r"a(\d+)=(\d+)")
STORAGE_RE = re.compile(r"FILED_STORAGE_PROFILE scope=(\S+)((?: \w+=\d+)+)")
STORAGE_ARG_RE = re.compile(r"(\w+)=(\d+)")


def name_id(value: str) -> int:
    result = FNV_OFFSET
    for byte in value.encode():
        result ^= byte
        result = (result * FNV_PRIME) & MASK64
    return result


def path_candidates(roots: list[Path]) -> dict[int, set[str]]:
    decoded: dict[int, set[str]] = defaultdict(set)
    explicit = [
        "/usr/bin/sway",
        "/usr/bin/foot",
        "/usr/bin/swaymsg",
        "/bin/bash",
        "/lib/ld-musl-x86_64.so.1",
    ]
    for candidate in explicit:
        decoded[name_id(candidate)].add(candidate)
    for root in roots:
        if not root.exists():
            continue
        for directory, dirs, files in os.walk(root):
            for name in dirs + files:
                path = Path(directory, name)
                relative = path.relative_to(root).as_posix()
                candidates = {f"/{relative}", relative, name}
                for candidate in candidates:
                    decoded[name_id(candidate)].add(candidate)
    return decoded


def add_manifest_candidates(decoded: dict[int, set[str]], manifests: list[Path]) -> None:
    for manifest in manifests:
        if not manifest.exists():
            continue
        for line in manifest.read_text(errors="replace").splitlines():
            if not line or line.startswith("#"):
                continue
            guest_path = line.split("=", 1)[0]
            if guest_path:
                decoded[name_id(guest_path)].add(guest_path)


def trace_records(path: Path):
    for line in path.read_text(errors="replace").splitlines():
        match = TRACE_RE.search(line)
        if match is None:
            continue
        args = {int(index): int(value) for index, value in ARG_RE.findall(match.group(3))}
        yield match.group(1), match.group(2), args


def storage_records(path: Path) -> dict[str, dict[str, int]]:
    records: dict[str, dict[str, int]] = {}
    for line in path.read_text(errors="replace").splitlines():
        match = STORAGE_RE.search(line)
        if match is None:
            continue
        values = {name: int(value) for name, value in STORAGE_ARG_RE.findall(match.group(2))}
        previous = records.get(match.group(1))
        if previous is None or values.get("total_cycles", values.get("cycles", 0)) >= previous.get(
            "total_cycles", previous.get("cycles", 0)
        ):
            records[match.group(1)] = values
    return records


@dataclass
class PathMetric:
    open_cycles: int = 0
    read_cycles: int = 0
    mmap_cycles: int = 0
    open_count: int = 0
    read_count: int = 0
    mmap_count: int = 0
    mmap_bytes: int = 0
    file_vmo_cycles: int = 0
    local_pread_cycles: int = 0
    file_vmo_count: int = 0
    local_pread_count: int = 0
    patch_cycles: int = 0
    patch_count: int = 0
    patch_bytes: int = 0
    patched_sites: int = 0
    skipped_sites: int = 0
    failed_sites: int = 0
    errors: int = 0

    @property
    def total_cycles(self) -> int:
        return self.open_cycles + self.read_cycles + self.mmap_cycles


def process_labels(console: Path | None) -> dict[int, str]:
    labels: dict[int, str] = {}
    if console is None or not console.exists():
        return labels
    text = console.read_text(errors="replace")
    for phase, pid in re.findall(r"P4_BENCH_SWAY_PID phase=(\S+) pid=(\d+)", text):
        labels[int(pid)] = f"sway:{phase}"
    for app, pid in re.findall(r"P4_BENCH_APP_PID app=(\S+) pid=(\d+)", text):
        labels[int(pid)] = app
    return labels


def display_path(path_hash: int, decoded: dict[int, set[str]]) -> str:
    names = sorted(decoded.get(path_hash, ()), key=lambda item: (not item.startswith("/"), len(item), item))
    return names[0] if names else f"hash:{path_hash}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", type=Path, required=True)
    parser.add_argument("--console", type=Path)
    parser.add_argument("--root", type=Path, action="append", default=[])
    parser.add_argument("--manifest", type=Path, action="append", default=[])
    parser.add_argument("--limit", type=int, default=15)
    args = parser.parse_args()

    decoded = path_candidates(args.root)
    add_manifest_candidates(decoded, args.manifest)
    labels = process_labels(args.console)
    cycles_marker = name_id("startup.path.cycles")
    counts_marker = name_id("startup.path.counts")
    bytes_marker = name_id("startup.path.bytes")
    backend_cycles_marker = name_id("startup.path.mmap_backend_cycles")
    backend_counts_marker = name_id("startup.path.mmap_backend_counts")
    patch_cycles_marker = name_id("startup.path.patch.cycles")
    patch_counts_marker = name_id("startup.path.patch.counts")
    patch_top_marker = name_id("startup.patch.path")
    patch_sites_marker = name_id("startup.patch.sites")
    stage_markers = {
        name_id("startup.mmap.stage.cycles.a"): ("cycles", 0, 3),
        name_id("startup.mmap.stage.cycles.b"): ("cycles", 3, 3),
        name_id("startup.mmap.stage.cycles.c"): ("cycles", 6, 2),
        name_id("startup.mmap.stage.counts.a"): ("counts", 0, 3),
        name_id("startup.mmap.stage.counts.b"): ("counts", 3, 3),
        name_id("startup.mmap.stage.counts.c"): ("counts", 6, 2),
    }
    stage_names = [
        "cache lookup",
        "FileD file_vmo RPC",
        "VMO create",
        "pread_to_vmo",
        "native mmap",
        "executable patch",
        "mprotect",
        "fallback copy",
    ]
    route_markers = {
        name_id("startup.mmap.route.counts.a"): (0, 4),
        name_id("startup.mmap.route.counts.b"): (4, 4),
        name_id("startup.mmap.route.counts.c"): (8, 4),
    }
    route_names = [
        "cache hit mapped",
        "cache mmap failed",
        "FileD VMO mapped",
        "FileD VMO RPC failed",
        "FileD VMO mmap failed",
        "no FileD backend",
        "no object generation",
        "empty file",
        "range overflow",
        "mapping past aligned file image",
        "local VMO/pread fallback",
        "file prefix / anonymous EOF tail optimized",
    ]
    filed_stage_markers = {
        name_id("filed.file_vmo.stage.cycles.a"): ("cycles", 0, 3),
        name_id("filed.file_vmo.stage.cycles.b"): ("cycles", 3, 3),
        name_id("filed.file_vmo.stage.cycles.c"): ("cycles", 6, 1),
        name_id("filed.file_vmo.stage.counts.a"): ("counts", 0, 3),
        name_id("filed.file_vmo.stage.counts.b"): ("counts", 3, 3),
        name_id("filed.file_vmo.stage.counts.c"): ("counts", 6, 1),
    }
    filed_stage_names = [
        "VFS prepare",
        "cache lookup",
        "cache miss create total",
        "VMO create",
        "server mmap",
        "cached pread",
        "reply FD transfer",
    ]
    metrics: dict[tuple[int, int], PathMetric] = defaultdict(PathMetric)
    mmap_stages: dict[int, dict[str, list[int]]] = defaultdict(
        lambda: {"cycles": [0] * len(stage_names), "counts": [0] * len(stage_names)}
    )
    mmap_routes: dict[int, list[int]] = defaultdict(lambda: [0] * len(route_names))
    file_vmo: dict[int, int] = {}
    filed_stages = {
        "cycles": [0] * len(filed_stage_names),
        "counts": [0] * len(filed_stage_names),
    }
    exec_first: dict[int, tuple[int, list[int]]] = {}
    exec_second: dict[int, list[int]] = {}
    storage = storage_records(args.serial)

    for component, event, record in trace_records(args.serial):
        marker = record.get(0)
        if component == "lpr" and event == "metric.timing_extra":
            pid = record.get(1, 0)
            if marker in stage_markers:
                kind, first, count = stage_markers[marker]
                for relative in range(count):
                    mmap_stages[pid][kind][first + relative] = record.get(2 + relative, 0)
                continue
            if marker in route_markers:
                first, count = route_markers[marker]
                for relative in range(count):
                    mmap_routes[pid][first + relative] = record.get(2 + relative, 0)
                continue
            if marker == patch_top_marker:
                metric = metrics[(pid, record.get(2, 0))]
                metric.patch_cycles = record.get(3, 0)
                metric.patch_count = record.get(4, 0)
                metric.patch_bytes = record.get(5, 0)
                continue
            if marker == patch_sites_marker:
                metrics[(pid, record.get(2, 0))].patched_sites = record.get(3, 0)
                continue
            if marker not in (
                cycles_marker,
                counts_marker,
                bytes_marker,
                backend_cycles_marker,
                backend_counts_marker,
                patch_cycles_marker,
                patch_counts_marker,
            ):
                continue
            metric = metrics[(pid, record.get(2, 0))]
            if marker == cycles_marker:
                metric.open_cycles = record.get(3, 0)
                metric.read_cycles = record.get(4, 0)
                metric.mmap_cycles = record.get(5, 0)
            elif marker == counts_marker:
                metric.open_count = record.get(3, 0)
                metric.read_count = record.get(4, 0)
                metric.mmap_count = record.get(5, 0)
            elif marker == bytes_marker:
                metric.mmap_bytes = record.get(3, 0)
                metric.errors = record.get(4, 0)
            elif marker == backend_cycles_marker:
                metric.file_vmo_cycles = record.get(3, 0)
                metric.local_pread_cycles = record.get(4, 0)
            elif marker == backend_counts_marker:
                metric.file_vmo_count = record.get(3, 0)
                metric.local_pread_count = record.get(4, 0)
            elif marker == patch_cycles_marker:
                metric.patch_cycles = record.get(3, 0)
                metric.patch_bytes = record.get(4, 0)
                metric.patched_sites = record.get(5, 0)
            elif marker == patch_counts_marker:
                metric.patch_count = record.get(3, 0)
                metric.skipped_sites = record.get(4, 0)
                metric.failed_sites = record.get(5, 0)
        elif component == "filed" and event == "metric.timing_extra" and marker in filed_stage_markers:
            kind, first, count = filed_stage_markers[marker]
            for relative in range(count):
                filed_stages[kind][first + relative] = record.get(1 + relative, 0)
        elif component == "filed" and event == "filed.metric.file_vmo":
            if 0 in record and 1 in record:
                file_vmo[record[0]] = record[1]
        elif component == "filed" and event == "filed.metric.exec":
            logical = record.get(0, 1 << 63)
            if logical < 4096 and len(record) == 6:
                exec_first[logical] = (record.get(1, 0), [record.get(i, 0) for i in range(2, 6)])
            elif logical < 4096 and len(record) == 3:
                exec_second[logical] = [record.get(1, 0), record.get(2, 0)]

    for pid in {key[0] for key in metrics}:
        hashes = {path_hash for row_pid, path_hash in metrics if row_pid == pid}
        if name_id("/usr/lib/libwlroots-0.19.so") in hashes:
            labels[pid] = "sway(native-pid)"
        elif name_id("/etc/xdg/foot/foot.ini") in hashes:
            labels[pid] = "foot(native-pid)"

    for pid in sorted({key[0] for key in metrics}):
        rows = [(path_hash, metric) for (row_pid, path_hash), metric in metrics.items() if row_pid == pid]
        rows.sort(key=lambda item: item[1].total_cycles, reverse=True)
        total = sum(metric.total_cycles for _, metric in rows)
        print(f"\n## process pid={pid} label={labels.get(pid, 'unknown')} profiled_cycles={total}")
        category_totals = {
            "open": sum(metric.open_cycles for _, metric in rows),
            "read/lseek": sum(metric.read_cycles for _, metric in rows),
            "mmap": sum(metric.mmap_cycles for _, metric in rows),
        }
        print("| operation | cycles | share |")
        print("|---|---:|---:|")
        for operation, cycles in category_totals.items():
            share = 100.0 * cycles / total if total else 0.0
            print(f"| {operation} | {cycles:,} | {share:.1f}% |")
        if pid in mmap_stages:
            stage_total = sum(mmap_stages[pid]["cycles"])
            print("\n| mmap stage | cycles | stage share | calls |")
            print("|---|---:|---:|---:|")
            for index, name in enumerate(stage_names):
                stage_cycles = mmap_stages[pid]["cycles"][index]
                stage_share = 100.0 * stage_cycles / stage_total if stage_total else 0.0
                print(
                    f"| {name} | {stage_cycles:,} | {stage_share:.1f}% | "
                    f"{mmap_stages[pid]['counts'][index]} |"
                )
        if pid in mmap_routes:
            print("\n| private file mmap route | count |")
            print("|---|---:|")
            for index, name in enumerate(route_names):
                print(f"| {name} | {mmap_routes[pid][index]} |")
        print()
        print("| path | share | open cycles/count | read cycles/count | mmap cycles/count | FileD VMO cycles/count | local pread cycles/count | patch cycles/count | patch MiB/sites | mmap MiB | errors |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for path_hash, metric in rows[: args.limit]:
            share = 100.0 * metric.total_cycles / total if total else 0.0
            print(
                f"| {display_path(path_hash, decoded)} | {share:.1f}% | "
                f"{metric.open_cycles:,}/{metric.open_count} | "
                f"{metric.read_cycles:,}/{metric.read_count} | "
                f"{metric.mmap_cycles:,}/{metric.mmap_count} | "
                f"{metric.file_vmo_cycles:,}/{metric.file_vmo_count} | "
                f"{metric.local_pread_cycles:,}/{metric.local_pread_count} | "
                f"{metric.patch_cycles:,}/{metric.patch_count} | "
                f"{metric.patch_bytes / (1024 * 1024):.2f}/{metric.patched_sites} | "
                f"{metric.mmap_bytes / (1024 * 1024):.2f} | {metric.errors} |"
            )

    if file_vmo:
        print("\n## FileD VMO cache")
        print("| metric | value |")
        print("|---|---:|")
        names = {1: "hits", 2: "misses", 3: "stores", 4: "evictions"}
        for key in sorted(file_vmo):
            print(f"| {names.get(key, str(key))} | {file_vmo[key]:,} |")
        lookups = file_vmo.get(1, 0) + file_vmo.get(2, 0)
        if lookups:
            print(f"\nVMO cache hit rate: {100.0 * file_vmo.get(1, 0) / lookups:.1f}%")
    if any(filed_stages["counts"]):
        total = sum(filed_stages["cycles"])
        print("\n## FileD file_vmo stages")
        print("| stage | cycles | measured share | calls |")
        print("|---|---:|---:|---:|")
        for index, name in enumerate(filed_stage_names):
            cycles = filed_stages["cycles"][index]
            share = 100.0 * cycles / total if total else 0.0
            print(f"| {name} | {cycles:,} | {share:.1f}% | {filed_stages['counts'][index]} |")

    fs_storage = storage.get("file_vmo_fs") or storage.get("fs")
    if fs_storage:
        total = fs_storage.get("total_cycles", 0)
        stages = [
            ("extent lookup", "extent_cycles", "extent_calls"),
            ("block-device read", "device_cycles", "device_calls"),
            ("dirty-cache overlay", "overlay_cycles", "overlay_calls"),
            ("unaligned partial copy", "partial_copy_cycles", "partial_copy_calls"),
        ]
        measured = sum(fs_storage.get(cycles_key, 0) for _, cycles_key, _ in stages)
        print("\n## FileD cached pread: Kobox ext4 stages")
        if "pread_calls" in fs_storage:
            print(
                f"file_vmo preads={fs_storage.get('pread_calls', 0):,}, "
                f"returned_bytes={fs_storage.get('pread_bytes', 0):,}"
            )
        print(
            f"calls={fs_storage.get('calls', 0):,}, "
            f"bytes={fs_storage.get('bytes', 0):,}, total_cycles={total:,}"
        )
        print("| stage | cycles | total share | calls |")
        print("|---|---:|---:|---:|")
        for name, cycles_key, calls_key in stages:
            cycles = fs_storage.get(cycles_key, 0)
            share = 100.0 * cycles / total if total else 0.0
            print(f"| {name} | {cycles:,} | {share:.1f}% | {fs_storage.get(calls_key, 0):,} |")
        residual = max(0, total - measured)
        share = 100.0 * residual / total if total else 0.0
        print(f"| setup/loop residual | {residual:,} | {share:.1f}% | — |")

    block_storage = storage.get("file_vmo_block") or storage.get("block")
    if block_storage:
        total = block_storage.get("total_cycles", 0)
        stages = [
            ("request allocation", "alloc_cycles", "alloc_calls"),
            ("DMA map", "map_cycles", "map_calls"),
            ("pre-submit setup", "before_cycles", "before_calls"),
            ("queue submission", "submit_cycles", "submit_calls"),
            ("completion wait", "wait_cycles", "wait_calls"),
            ("request free total", "free_cycles", "free_calls"),
        ]
        measured = sum(block_storage.get(cycles_key, 0) for _, cycles_key, _ in stages)
        print("\n## FileD cached pread: NVMe synchronous stages")
        print(
            f"calls={block_storage.get('total_calls', 0):,}, "
            f"bytes={block_storage.get('bytes', 0):,}, total_cycles={total:,}"
        )
        print("| stage | cycles | total share | calls |")
        print("|---|---:|---:|---:|")
        for name, cycles_key, calls_key in stages:
            cycles = block_storage.get(cycles_key, 0)
            share = 100.0 * cycles / total if total else 0.0
            print(f"| {name} | {cycles:,} | {share:.1f}% | {block_storage.get(calls_key, 0):,} |")
        residual = max(0, total - measured)
        share = 100.0 * residual / total if total else 0.0
        print(f"| command setup/residual | {residual:,} | {share:.1f}% | — |")
        unmap = block_storage.get("unmap_cycles", 0)
        unmap_share = 100.0 * unmap / total if total else 0.0
        print(
            f"\nDMA unmap/copy-back is nested in request free: {unmap:,} cycles "
            f"({unmap_share:.1f}%), {block_storage.get('unmap_calls', 0):,} calls."
        )
        cq_poll = block_storage.get("cq_poll_cycles", 0)
        irq_wait = block_storage.get("irq_wait_cycles", 0)
        poll_yield = block_storage.get("poll_yield_cycles", 0)
        post_irq = block_storage.get("post_irq_cycles", 0)
        if cq_poll or irq_wait or poll_yield or post_irq:
            completion_total = block_storage.get("wait_cycles", 0)
            print("\n### NVMe completion-wait detail")
            print("| stage | cycles | completion share | calls |")
            print("|---|---:|---:|---:|")
            for name, cycles, calls in [
                ("CQ poll/spin", cq_poll, block_storage.get("cq_poll_calls", 0)),
                ("IRQ signal wait", irq_wait, block_storage.get("irq_wait_calls", 0)),
                ("CQ poll yield", poll_yield, block_storage.get("poll_yield_calls", 0)),
            ]:
                share = 100.0 * cycles / completion_total if completion_total else 0.0
                print(f"| {name} | {cycles:,} | {share:.1f}% | {calls:,} |")
            post_share = 100.0 * post_irq / completion_total if completion_total else 0.0
            print(
                f"\nPost-IRQ CQ drain is nested in CQ poll/spin: {post_irq:,} cycles "
                f"({post_share:.1f}%), {block_storage.get('post_irq_calls', 0):,} calls."
            )
        map_detail = [
            ("PRP allocation/init", "prp_alloc_cycles", "prp_alloc_calls"),
            ("data page map", "data_map_cycles", "data_map_calls"),
            ("PRP validate/build", "prp_build_cycles", "prp_build_calls"),
            ("PRP auxiliary map", "prp_aux_map_cycles", "prp_aux_map_calls"),
        ]
        if any(block_storage.get(cycles_key, 0) for _, cycles_key, _ in map_detail):
            map_total = block_storage.get("map_cycles", 0)
            print("\n### NVMe DMA-map detail")
            print("| stage | cycles | DMA-map share | calls |")
            print("|---|---:|---:|---:|")
            measured_map = 0
            for name, cycles_key, calls_key in map_detail:
                cycles = block_storage.get(cycles_key, 0)
                measured_map += cycles
                share = 100.0 * cycles / map_total if map_total else 0.0
                print(f"| {name} | {cycles:,} | {share:.1f}% | {block_storage.get(calls_key, 0):,} |")
            residual = max(0, map_total - measured_map)
            share = 100.0 * residual / map_total if map_total else 0.0
            print(f"| wrapper/residual | {residual:,} | {share:.1f}% | — |")
            if any(block_storage.get(key, 0) for key in (
                "prp_cache_hit_calls",
                "prp_cache_miss_calls",
                "prp_cache_fallback_calls",
            )):
                print("\nPRP auxiliary cache detail (nested in PRP auxiliary map):")
                print("| path | cycles | calls |")
                print("|---|---:|---:|")
                for name, cycles_key, calls_key in [
                    ("cache hit", "prp_cache_hit_cycles", "prp_cache_hit_calls"),
                    ("cache miss/allocation", "prp_cache_miss_cycles", "prp_cache_miss_calls"),
                    ("fallback map", "prp_cache_fallback_cycles", "prp_cache_fallback_calls"),
                ]:
                    print(
                        f"| {name} | {block_storage.get(cycles_key, 0):,} | "
                        f"{block_storage.get(calls_key, 0):,} |"
                    )

    irq_storage = storage.get("file_vmo_irq") or storage.get("irq")
    if irq_storage:
        wait_total = irq_storage.get("wait_cycles", 0)
        stages = [("fd_wait_many", "fd_wait_cycles", "fd_wait_calls")]
        if "pre_poll_cycles" in irq_storage or "post_poll_cycles" in irq_storage:
            stages.extend([
                ("IRQ count pre-poll", "pre_poll_cycles", "pre_poll_calls"),
                ("IRQ count post-poll", "post_poll_cycles", "post_poll_calls"),
            ])
        else:
            stages.append(("IRQ count poll", "poll_cycles", "poll_calls"))
        stages.append(("IRQ callback", "handler_cycles", "handler_calls"))
        measured = sum(irq_storage.get(cycles_key, 0) for _, cycles_key, _ in stages)
        print("\n### PachaOS IRQ backend detail")
        print(
            f"waits={irq_storage.get('wait_calls', 0):,}, "
            f"total_cycles={wait_total:,}, fd_ready={irq_storage.get('fd_wait_ready', 0):,}, "
            f"pre_ready={irq_storage.get('pre_poll_ready', 0):,}, "
            f"post_ready={irq_storage.get('post_poll_ready', 0):,}"
        )
        print("| stage | cycles | backend-wait share | calls |")
        print("|---|---:|---:|---:|")
        for name, cycles_key, calls_key in stages:
            cycles = irq_storage.get(cycles_key, 0)
            share = 100.0 * cycles / wait_total if wait_total else 0.0
            print(f"| {name} | {cycles:,} | {share:.1f}% | {irq_storage.get(calls_key, 0):,} |")
        residual = max(0, wait_total - measured)
        residual_share = 100.0 * residual / wait_total if wait_total else 0.0
        print(f"| wrapper/residual | {residual:,} | {residual_share:.1f}% | — |")

    dma_copy = storage.get("file_vmo_dma_copy") or storage.get("dma_copy")
    if dma_copy:
        print(
            "\nPachaOS DMA bounce copy-back: "
            f"{dma_copy.get('calls', 0):,} calls, "
            f"{dma_copy.get('bytes', 0):,} bytes, "
            f"{dma_copy.get('cycles', 0):,} cycles."
        )

    dma_window = storage.get("dma_window")
    if dma_window:
        print(
            "\nFileD pread DMA windows: "
            f"{dma_window.get('begin_calls', 0):,} begins, "
            f"{dma_window.get('mapping_calls', 0):,} kernel mappings, "
            f"{dma_window.get('mapping_cycles', 0):,} mapping cycles, "
            f"{dma_window.get('reuse_calls', 0):,} request reuses, "
            f"{dma_window.get('reuse_cycles', 0):,} reuse cycles, "
            f"{dma_window.get('end_calls', 0):,} ends, "
            f"{dma_window.get('mapped_bytes', 0):,} mapped bytes, "
            f"{dma_window.get('direct_mapping_calls', 0):,} direct mappings / "
            f"{dma_window.get('direct_mapping_cycles', 0):,} cycles / "
            f"{dma_window.get('direct_mapped_bytes', 0):,} bytes, "
            f"{dma_window.get('staged_read_calls', 0):,} staged reads / "
            f"{dma_window.get('staged_bytes', 0):,} bytes, "
            f"{dma_window.get('staging_copy_cycles', 0):,} copy cycles."
        )

    if exec_first:
        print("\n## FileD exec")
        print("| logical | path | total cycles | inherit | init main | read meta | load plan | start plan |")
        print("|---:|---|---:|---:|---:|---:|---:|---:|")
        for logical in sorted(exec_first):
            path_hash, first = exec_first[logical]
            second = exec_second.get(logical, [0, 0])
            print(
                f"| {logical} | {display_path(path_hash, decoded)} | "
                f"{first[0]:,} | {first[1]:,} | {first[2]:,} | {first[3]:,} | "
                f"{second[0]:,} | {second[1]:,} |"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
