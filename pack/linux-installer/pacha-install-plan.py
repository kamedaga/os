#!/usr/bin/env python3
"""Plan additions to an existing GPT and verify preservation before formatting."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import uuid

GIB = 1 << 30
MIB = 1 << 20
ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
LINUX = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
MICROSOFT = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"


def checked_table(document, total_bytes):
    table = document["partitiontable"]
    if table.get("label") != "gpt" or table.get("unit") != "sectors":
        raise ValueError("An existing GPT disk is required; no disk conversion is performed.")
    uuid.UUID(table["id"])
    sector = table["sectorsize"]
    first, last = table["firstlba"], table["lastlba"]
    if sector not in (512, 4096) or not 0 < first <= last < total_bytes // sector:
        raise ValueError("Invalid GPT geometry.")
    parts = sorted(table.get("partitions", []), key=lambda part: part["start"])
    cursor = first
    nodes, identities = set(), set()
    for part in parts:
        start, size = part["start"], part["size"]
        identity = str(uuid.UUID(part["uuid"]))
        uuid.UUID(part["type"])
        if (not isinstance(start, int) or not isinstance(size, int) or
                start < cursor or size <= 0 or start + size > last + 1 or
                part["node"] in nodes or identity in identities):
            raise ValueError("Invalid or overlapping existing partitions.")
        nodes.add(part["node"])
        identities.add(identity)
        cursor = start + size
    return table, parts


def make_plan(document, total_bytes, root_gib=None):
    table, parts = checked_table(document, total_bytes)
    types = {part["type"].upper() for part in parts}
    if not {ESP, MICROSOFT}.issubset(types):
        raise ValueError("Windows coexistence requires existing EFI and Microsoft data partitions.")
    if any(part.get("name", "").startswith("PachaOS") for part in parts):
        raise ValueError("An existing PachaOS partition was found; refusing a duplicate installation.")
    if root_gib is not None and not 16 <= root_gib <= 4096:
        raise ValueError("Root size must be between 16 and 4096 GiB.")
    sector = table["sectorsize"]
    alignment = MIB // sector
    esp_size = 512 * MIB // sector
    # Only holes between validated GPT extents are eligible. Filesystem free
    # space is never considered, and no existing extent is moved or resized.
    gaps = []
    cursor = table["firstlba"]
    for part in parts + [{"start": table["lastlba"] + 1, "size": 0}]:
        start = ((cursor + alignment - 1) // alignment) * alignment
        end = (part["start"] // alignment) * alignment
        if end > start:
            gaps.append((start, end))
        cursor = part["start"] + part["size"]
    needed = esp_size + (root_gib or 16) * GIB // sector
    eligible = [(start, end) for start, end in gaps if end - start >= needed]
    if not eligible:
        raise ValueError("No sufficiently large UNALLOCATED region. Create at least "
                         f"{root_gib or 16}.5 GiB of unallocated space in Windows first; "
                         "this installer does not shrink Windows.")
    start, end = max(eligible, key=lambda gap: (gap[1] - gap[0], -gap[0]))
    root_size = (root_gib * GIB // sector if root_gib else
                 min(64 * GIB // sector, end - start - esp_size))
    additions = [
        {"start": start, "size": esp_size, "type": ESP,
         "uuid": str(uuid.uuid4()), "name": "PachaOS EFI"},
        {"start": start + esp_size, "size": root_size, "type": LINUX,
         "uuid": str(uuid.uuid4()), "name": "PachaOS root"},
    ]
    return {"before": document, "disk_bytes": total_bytes, "additions": additions}


def append_script(plan):
    header = ""
    if plan.get("mode") == "erase-disk":
        table = plan["before"]["partitiontable"]
        header = (f'label: gpt\nlabel-id: {table["id"]}\nunit: sectors\n'
                  f'first-lba: {table["firstlba"]}\nlast-lba: {table["lastlba"]}\n')
    return header + "".join(f'start={p["start"]},size={p["size"]},type={p["type"]},'
                   f'uuid={p["uuid"]},name="{p["name"]}"\n' for p in plan["additions"])


def make_erase_plan(device, total_bytes, sector, root_gib=None):
    if sector not in (512, 4096) or total_bytes % sector:
        raise ValueError("Invalid disk sector geometry.")
    if root_gib is not None and not 16 <= root_gib <= 4096:
        raise ValueError("Root size must be between 16 and 4096 GiB.")
    alignment = MIB // sector
    # Reserve a standard 128-entry GPT at both ends of the device.
    last = total_bytes // sector - (16384 + sector - 1) // sector - 2
    start = alignment
    esp_size = 512 * MIB // sector
    end = (last + 1) // alignment * alignment
    available = end - start - esp_size
    root_size = root_gib * GIB // sector if root_gib is not None else available
    if root_size < 16 * GIB // sector or root_size > available:
        raise ValueError("Disk cannot fit the requested root (minimum 16 GiB) and 512 MiB EFI partition.")
    table = {"label": "gpt", "id": str(uuid.uuid4()).upper(), "device": device,
             "unit": "sectors", "firstlba": start, "lastlba": last,
             "sectorsize": sector, "partitions": []}
    return {"mode": "erase-disk", "before": {"partitiontable": table},
            "disk_bytes": total_bytes, "additions": [
                {"start": start, "size": esp_size, "type": ESP,
                 "uuid": str(uuid.uuid4()), "name": "PachaOS EFI"},
                {"start": start + esp_size, "size": root_size, "type": LINUX,
                 "uuid": str(uuid.uuid4()), "name": "PachaOS root"}]}


def metadata_fingerprint(device, total_bytes):
    digest = hashlib.sha256()
    length = min(MIB, total_bytes)
    with open(device, "rb", buffering=0) as stream:
        for offset in (0, total_bytes - length):
            stream.seek(offset)
            data = stream.read(length)
            if len(data) != length:
                raise ValueError("Short disk metadata read.")
            digest.update(data)
    return digest.hexdigest()


def verify_after(plan, document):
    old, before = checked_table(plan["before"], plan["disk_bytes"])
    new, after = checked_table(document, plan["disk_bytes"])
    for field in ("label", "id", "device", "unit", "firstlba", "lastlba", "sectorsize"):
        if old.get(field) != new.get(field):
            raise ValueError("Disk identity or GPT geometry changed: " + field)
    indexed = {part["node"]: part for part in after}
    for part in before:
        if indexed.get(part["node"]) != part:
            raise ValueError("An existing partition changed: " + part["node"])
    existing = {part["node"] for part in before}
    added = [part for part in after if part["node"] not in existing]
    if len(added) != 2:
        raise ValueError("Expected exactly two new partitions.")
    resolved = []
    for expected in plan["additions"]:
        matches = [part for part in added if part["uuid"].lower() == expected["uuid"].lower()]
        if len(matches) != 1:
            raise ValueError("New partition identity does not match the plan.")
        actual = matches[0]
        for field in ("start", "size", "name"):
            if actual.get(field) != expected[field]:
                raise ValueError("New partition geometry/name differs from the plan.")
        if actual["type"].upper() != expected["type"]:
            raise ValueError("New partition type differs from the plan.")
        # Recheck non-overlap independently of the writer's table parser.
        if any(actual["start"] < p["start"] + p["size"] and
               p["start"] < actual["start"] + actual["size"] for p in before):
            raise ValueError("New partition overlaps an existing partition.")
        resolved.append(actual["node"])
    return resolved


def boot_state(text):
    match = re.search(r"^BootOrder: ([0-9A-Fa-f,]+)$", text, re.M)
    entries = dict(re.findall(r"^Boot([0-9A-Fa-f]{4})([^\n]*)$", text, re.M))
    order = match[1].upper().split(",") if match else []
    entries = {key.upper(): value for key, value in entries.items()}
    if not order or len(order) != len(set(order)) or not set(order).issubset(entries):
        raise ValueError("Existing UEFI BootOrder is missing or invalid.")
    return order, entries


def added_boot_order(before_text, after_text, first=False):
    order, before = boot_state(before_text)
    current_order, after = boot_state(after_text)
    if order != current_order or any(after.get(key) != value for key, value in before.items()):
        raise ValueError("Existing UEFI entries or order changed unexpectedly.")
    added = set(after) - set(before)
    if len(added) != 1:
        raise ValueError("Expected one newly created UEFI entry.")
    entry = added.pop()
    if not re.match(r"\*?\s+PachaOS(?:\s|$)", after[entry]):
        raise ValueError("New UEFI entry is not PachaOS.")
    return ",".join([entry] + order if first else order + [entry])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    plan = sub.add_parser("plan")
    plan.add_argument("table", type=Path)
    plan.add_argument("disk_bytes", type=int)
    plan.add_argument("output", type=Path)
    plan.add_argument("script", type=Path)
    plan.add_argument("--root-size-gib", type=int)
    erase = sub.add_parser("erase-plan")
    erase.add_argument("device")
    erase.add_argument("disk_bytes", type=int)
    erase.add_argument("sector", type=int)
    erase.add_argument("output", type=Path)
    erase.add_argument("script", type=Path)
    erase.add_argument("--root-size-gib", type=int)
    fingerprint = sub.add_parser("fingerprint")
    fingerprint.add_argument("device")
    fingerprint.add_argument("disk_bytes", type=int)
    verify = sub.add_parser("verify")
    verify.add_argument("plan", type=Path)
    verify.add_argument("table", type=Path)
    check = sub.add_parser("boot-check")
    check.add_argument("before", type=Path)
    boot = sub.add_parser("boot-order")
    boot.add_argument("before", type=Path)
    boot.add_argument("after", type=Path)
    boot.add_argument("--first", action="store_true")
    args = parser.parse_args()
    try:
        if args.command in ("plan", "erase-plan"):
            if args.command == "plan":
                result = make_plan(json.loads(args.table.read_text()), args.disk_bytes, args.root_size_gib)
            else:
                result = make_erase_plan(args.device, args.disk_bytes, args.sector, args.root_size_gib)
            args.output.write_text(json.dumps(result, indent=2) + "\n")
            args.script.write_text(append_script(result))
            sector = result["before"]["partitiontable"]["sectorsize"]
            print("ERASE DISK: all existing partitions, including Windows and recovery, will be removed."
                  if args.command == "erase-plan" else
                  "Existing partitions will be preserved. Additions in UNALLOCATED space:")
            for part in result["additions"]:
                print(f'  {part["name"]}: {part["size"] * sector / GIB:.3f} GiB, '
                      f'start sector {part["start"]}, partition UUID {part["uuid"]}')
        elif args.command == "fingerprint":
            print(metadata_fingerprint(args.device, args.disk_bytes))
        elif args.command == "verify":
            print("\n".join(verify_after(json.loads(args.plan.read_text()), json.loads(args.table.read_text()))))
        elif args.command == "boot-check":
            boot_state(args.before.read_text())
        else:
            print(added_boot_order(args.before.read_text(), args.after.read_text(), args.first))
    except (ValueError, KeyError, TypeError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
