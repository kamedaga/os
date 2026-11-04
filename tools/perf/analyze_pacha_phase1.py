#!/usr/bin/env python3
"""Summarize PachaOS phase-1 terminal and Minecraft artifacts as JSON."""
import argparse
import datetime as dt
import json
import re
import statistics
from pathlib import Path


def values(paths, pattern):
    found = []
    for path in paths:
        match = re.search(pattern, path.read_text(errors="replace"))
        if match:
            found.append(float(match.group(1)))
    return found


def summary(samples):
    return {"runs": samples, "median": statistics.median(samples),
            "min": min(samples), "max": max(samples)}


def log_seconds(text, needle):
    match = re.search(r"\[(\d\d):(\d\d):(\d\d)\].*" + re.escape(needle), text)
    if not match:
        return None
    hour, minute, second = map(int, match.groups())
    return hour * 3600 + minute * 60 + second


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact_root", type=Path)
    parser.add_argument("--verbosegc", type=Path)
    args = parser.parse_args()
    root = args.artifact_root
    terminal = root / "terminal"
    starts = values(terminal.glob("start-*.txt"), r"start_to_map_ms=([0-9.]+)")
    seq = values(terminal.glob("output-seq-*.txt-*.txt"), r"start_to_file_ms=([0-9.]+)")
    block = values(terminal.glob("output-block-*.txt-*.txt"), r"start_to_file_ms=([0-9.]+)")
    minecraft = root / "final/minecraft-1"
    log = (minecraft / "latest.log").read_text(errors="replace")
    events = {
        "fabric_main": "Loading Minecraft",
        "render_environment": "Environment:",
        "resource_reload": "Reloading ResourceManager:",
        "last_atlas": "Created: 512x512x0 minecraft:textures/atlas/shulker_boxes.png-atlas",
        "title_ready": "Loaded 1617 advancements",
        "world_start": "Starting integrated minecraft server",
        "world_ready": "Started 2 worker threads",
        "world_saved": "ThreadedAnvilChunkStorage: All dimensions are saved",
        "render_stop_begin": "Stopping worker threads",
        "render_stopped": "Stopping!",
    }
    stamps = {name: log_seconds(log, needle) for name, needle in events.items()}
    start_epoch = int((minecraft / "start-epoch.txt").read_text())
    # Guest log4j timestamps are UTC while the controller runs in Asia/Tokyo.
    start_time = dt.datetime.fromtimestamp(start_epoch, dt.timezone.utc)
    start_local = start_time.hour * 3600 + start_time.minute * 60 + start_time.second
    map_ms = values([minecraft / "window-map.txt"], r"start_to_map_ms=([0-9.]+)")[0]
    phases = {"launcher_to_window_map_s": map_ms / 1000,
              "launcher_to_fabric_main_s": stamps["fabric_main"] - start_local,
              "fabric_main_to_render_environment_s": stamps["render_environment"] - stamps["fabric_main"],
              "render_environment_to_resource_reload_s": stamps["resource_reload"] - stamps["render_environment"],
              "resource_reload_to_last_atlas_s": stamps["last_atlas"] - stamps["resource_reload"],
              "last_atlas_to_title_ready_s": stamps["title_ready"] - stamps["last_atlas"],
              "title_ready_to_world_start_s": stamps["world_start"] - stamps["title_ready"],
              "world_start_to_world_ready_s": stamps["world_ready"] - stamps["world_start"],
              "launcher_to_title_ready_s": stamps["title_ready"] - start_local,
              "fabric_main_to_title_ready_s": stamps["title_ready"] - stamps["fabric_main"],
              "title_ready_to_world_ready_s": stamps["world_ready"] - stamps["title_ready"],
              "launcher_to_world_ready_s": stamps["world_ready"] - start_local,
              "render_stop_wait_s": stamps["render_stopped"] - stamps["render_stop_begin"]}
    core = (minecraft / "javacore.txt").read_text(errors="replace")
    jvm_start_match = re.search(
        r"JVM start time: \d{4}/\d{2}/\d{2} at (\d\d):(\d\d):(\d\d):(\d{3})",
        core)
    if jvm_start_match:
        hour, minute, second, millisecond = map(int, jvm_start_match.groups())
        jvm_start = hour * 3600 + minute * 60 + second + millisecond / 1000
        phases["launcher_to_jvm_start_s"] = round(jvm_start - start_local, 3)
        phases["jvm_start_to_fabric_main_s"] = round(stamps["fabric_main"] - jvm_start, 3)
    class_counts = [int(value) for value in re.findall(r"Number of loaded classes (\d+)", core)]
    gc_counts = re.search(r"globalcount=(\d+) scavengecount=(\d+)", core)
    result = {
        "terminal": {"start_to_map_ms": summary(starts),
                     "seq_200000_start_to_done_ms": summary(seq),
                     "block_8493498_bytes_start_to_done_ms": summary(block)},
        "minecraft": {"phases": phases, "loaded_classes": sum(class_counts),
                      "class_loader_counts": class_counts,
                      "gc_global_count": int(gc_counts.group(1)) if gc_counts else None,
                      "gc_scavenge_count": int(gc_counts.group(2)) if gc_counts else None},
    }
    if args.verbosegc:
        verbose = args.verbosegc.read_text(errors="replace")
        pauses = [float(value) for value in re.findall(r"<exclusive-end[^>]*durationms=\"([0-9.]+)\"", verbose)]
        result["minecraft"]["verbosegc_diagnostic"] = {
            "pause_count": len(pauses), "pause_total_ms": sum(pauses),
            "pause_max_ms": max(pauses), "pause_median_ms": statistics.median(pauses)}
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
