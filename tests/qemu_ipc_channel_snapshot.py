#!/usr/bin/env python3
"""Count live IPC channels from a paused VM, without kernel instrumentation.

--elf must describe the running kernel exactly (boot that unstripped ELF, or
its stripped copy). Separately rebuilding with debug flags is not sufficient.
Pause/resume is deliberately left to the caller. Without --qmp, only inspect
an existing --dump. Snapshots belong under .artifacts/.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

from qemu_xfce_apk_add_smoke import QMP


def dwarf_entries(elf):
    output = subprocess.check_output(["readelf", "--debug-dump=info", str(elf)], text=True)
    entries, current = {}, None
    for line in output.splitlines():
        match = re.match(r"\s*<(\d+)><([0-9a-f]+)>:.*\(DW_TAG_(\w+)\)", line)
        if match:
            current = {"depth": int(match[1]), "tag": match[3]}
            entries[int(match[2], 16)] = current
            continue
        match = re.search(r"DW_AT_(\w+)\s*:\s*(.*)", line)
        if match and current is not None:
            current[match[1]] = match[2]
    return entries


def name(entry):
    return entry.get("name", "").rsplit(": ", 1)[-1]


def number(value):
    return int(value, 0)


def child(entries, parent, field):
    depth = entries[parent]["depth"]
    for offset, entry in entries.items():
        if offset <= parent:
            continue
        if entry["depth"] <= depth:
            break
        if entry["depth"] == depth + 1 and name(entry) == field:
            return entry
    raise ValueError(f"missing member {field}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--dump", required=True, type=Path)
    parser.add_argument("--qmp", help="save a new snapshot; VM must already be paused")
    args = parser.parse_args()
    entries = dwarf_entries(args.elf)

    def structure(type_name):
        return next(offset for offset, entry in entries.items()
                    if entry["tag"] == "structure_type" and name(entry) == type_name)

    state = structure("kernel.KernelState")
    slot = structure("state.types.IpcChannelSlot")
    queue = structure("state.types.IpcQueue")
    field = child(entries, state, "ipc_channels")
    array = int(re.fullmatch(r"<0x([0-9a-f]+)>", field["type"])[1], 16)
    capacity = next(number(entry["count"]) for offset, entry in entries.items()
                    if offset > array and entry["tag"] == "subrange_type")
    slot_size = number(entries[slot]["byte_size"])
    queue_size = number(entries[queue]["byte_size"])
    active_offset = number(child(entries, slot, "active")["data_member_location"])
    refs_offset = number(child(entries, slot, "ref_count")["data_member_location"])
    queues_offset = number(child(entries, slot, "queues")["data_member_location"])
    queue_len_offset = number(child(entries, queue, "len")["data_member_location"])
    symbols = subprocess.check_output(["nm", "-n", str(args.elf)], text=True)
    state_address = next(int(line.split()[0], 16) for line in symbols.splitlines()
                         if line.endswith(" boot.entry.limine_kernel_state_storage"))
    address = state_address + number(field["data_member_location"])
    size = capacity * slot_size
    if args.qmp:
        if args.dump.exists():
            parser.error("snapshot already exists; select a new --dump path")
        qmp = QMP(args.qmp)
        try:
            if qmp.execute("query-status")["running"]:
                parser.error("pause the VM at the chosen checkpoint before capturing")
            command = f"memsave 0x{address:x} {size} {json.dumps(str(args.dump.resolve()))}"
            result = qmp.execute("human-monitor-command", {"command-line": command})
            if result:
                raise RuntimeError(result)
        finally:
            qmp.close()
    data = args.dump.read_bytes()
    if len(data) != size:
        raise ValueError(f"snapshot size {len(data)} != expected {size}")
    active = queued = 0
    refs = [0, 0, 0, 0]
    for offset in range(0, size, slot_size):
        if not data[offset + active_offset]:
            continue
        active += 1
        refs[min(data[offset + refs_offset], 3)] += 1
        for side in range(2):
            queued += data[offset + queues_offset + side * queue_size + queue_len_offset]
    print(json.dumps({"active": active, "capacity": capacity, "refs0": refs[0],
                      "refs1": refs[1], "refs2": refs[2], "refs_other": refs[3],
                      "queued": queued, "slot_bytes": slot_size}))


if __name__ == "__main__":
    main()
