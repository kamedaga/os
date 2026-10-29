#!/usr/bin/env python3
"""Partition preservation tests; all disk writes target disposable regular files."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("coexist", ROOT / "pack/linux-installer/pacha-install-plan.py")
planner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(planner)


def table(sector=512):
    mib = planner.MIB // sector
    parts = []
    for index, (start, size, kind) in enumerate([
        (1, 128, planner.ESP), (129, 16, "E3C9E316-0B5C-4DB8-817D-F92DF00215AE"),
        (145, 2048, planner.MICROSOFT), (2193, 512, "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"),
    ], 1):
        parts.append({"node": f"/dev/nvme0n1p{index}", "start": start*mib,
                      "size": size*mib, "type": kind,
                      "uuid": f"00000000-0000-4000-8000-{index:012d}",
                      "name": ("Windows", "MSR", "Data", "Recovery")[index-1]})
    return {"partitiontable": {"label": "gpt", "id": "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE",
            "device": "/dev/nvme0n1", "unit": "sectors", "firstlba": mib,
            "lastlba": 128*planner.GIB//sector-34, "sectorsize": sector, "partitions": parts}}


def after_plan(plan):
    result = copy.deepcopy(plan["before"])
    for number, part in enumerate(plan["additions"], 5):
        result["partitiontable"]["partitions"].append(dict(part, node=f"/dev/nvme0n1p{number}"))
    return result


class Planning(unittest.TestCase):
    def test_erase_uses_new_gpt_and_all_remaining_capacity_for_both_sector_sizes(self):
        for sector in (512, 4096):
            plan = planner.make_erase_plan("/dev/nvme0n1", 128*planner.GIB, sector)
            after = after_plan(plan)
            self.assertEqual(len(planner.verify_after(plan, after)), 2)
            root = plan["additions"][1]
            self.assertGreater(root["size"]*sector, 127*planner.GIB)
            self.assertLessEqual((root["start"]+root["size"])*sector, 128*planner.GIB)
            self.assertTrue(planner.append_script(plan).startswith("label: gpt\nlabel-id: "))
            after["partitiontable"]["id"] = str(uuid.uuid4())
            with self.assertRaises(ValueError): planner.verify_after(plan, after)

    def test_erase_requested_size_and_invalid_geometry(self):
        plan = planner.make_erase_plan("/dev/nvme0n1", 128*planner.GIB, 512, 24)
        self.assertEqual(plan["additions"][1]["size"]*512, 24*planner.GIB)
        for total, sector, size in ((16*planner.GIB, 512, None), (128*planner.GIB, 512, 128),
                                    (128*planner.GIB, 512, 15), (128*planner.GIB, 1024, None),
                                    (128*planner.GIB+1, 4096, None)):
            with self.assertRaises(ValueError):
                planner.make_erase_plan("/dev/nvme0n1", total, sector, size)

    def test_512_and_4k_sectors_preserve_all_existing_extents(self):
        for sector in (512, 4096):
            before = table(sector)
            saved = copy.deepcopy(before)
            plan = planner.make_plan(before, 128*planner.GIB)
            self.assertEqual(before, saved)
            self.assertEqual(plan["additions"][0]["size"]*sector, 512*planner.MIB)
            self.assertEqual(plan["additions"][1]["size"]*sector, 64*planner.GIB)
            self.assertEqual(planner.verify_after(plan, after_plan(plan)),
                             ["/dev/nvme0n1p5", "/dev/nvme0n1p6"])

    def test_existing_geometry_identity_attrs_and_names_cannot_change(self):
        plan = planner.make_plan(table(), 128*planner.GIB)
        for field, value in [("start", 4096), ("size", 1), ("uuid", "11111111-2222-3333-4444-555555555555"),
                             ("name", "changed"), ("type", planner.LINUX), ("attrs", "RequiredPartition")]:
            after = after_plan(plan)
            after["partitiontable"]["partitions"][0][field] = value
            with self.assertRaises(ValueError, msg=field):
                planner.verify_after(plan, after)

    def test_missing_mbr_full_overlapping_duplicate_and_existing_pacha_are_rejected(self):
        mutations = [
            lambda t: t.update(label="dos"),
            lambda t: t.update(partitions=[]),
            lambda t: t.update(lastlba=2706*2048),
            lambda t: t["partitions"][1].update(start=2048),
            lambda t: t["partitions"][1].update(uuid=t["partitions"][0]["uuid"]),
            lambda t: t["partitions"][0].update(name="PachaOS EFI"),
        ]
        for change in mutations:
            before = table(); change(before["partitiontable"])
            with self.assertRaises(ValueError):
                planner.make_plan(before, 128*planner.GIB)

    def test_explicit_size_and_insufficient_gap(self):
        plan = planner.make_plan(table(), 128*planner.GIB, 24)
        self.assertEqual(plan["additions"][1]["size"]*512, 24*planner.GIB)
        for size in (0, 15, 128, 4097):
            with self.assertRaises(ValueError):
                planner.make_plan(table(), 128*planner.GIB, size)

    def test_gap_before_recovery_is_usable_without_moving_recovery(self):
        before = table()
        before["partitiontable"]["partitions"][-1]["start"] = 127*planner.GIB//512
        plan = planner.make_plan(before, 128*planner.GIB)
        self.assertLess(sum(plan["additions"][1][key] for key in ("start", "size")),
                        before["partitiontable"]["partitions"][-1]["start"])

    def test_tampered_new_partition_or_extra_partition_is_rejected(self):
        plan = planner.make_plan(table(), 128*planner.GIB)
        for field, value in [("size", 100), ("start", 2048), ("uuid", "11111111-2222-3333-4444-555555555555")]:
            after = after_plan(plan); after["partitiontable"]["partitions"][-1][field] = value
            with self.assertRaises(ValueError): planner.verify_after(plan, after)
        after = after_plan(plan); after["partitiontable"]["partitions"].pop()
        with self.assertRaises(ValueError): planner.verify_after(plan, after)

    def test_boot_order_preserves_windows_default_and_all_existing_entries(self):
        before = "BootCurrent: 0002\nBootOrder: 0001,0002\nBoot0001* Windows Boot Manager\tHD(1)\nBoot0002* USB\tUSB(1)\n"
        after = before + "Boot0003* PachaOS\tHD(5)\n"
        self.assertEqual(planner.added_boot_order(before, after), "0001,0002,0003")
        self.assertEqual(planner.added_boot_order(before, after, first=True), "0003,0001,0002")
        for wrong in (after.replace("0001,0002", "0002,0001"),
                      after.replace("Windows Boot Manager", "changed"),
                      after.replace("PachaOS", "unexpected"), before,
                      after+"Boot0004* other\n"):
            with self.assertRaises(ValueError): planner.added_boot_order(before, wrong)


@unittest.skipUnless(shutil.which("sfdisk"), "sfdisk unavailable")
class FileBackedGPT(unittest.TestCase):
    def test_erase_dry_run_is_readonly_and_replaces_blank_mbr_and_gpt(self):
        with tempfile.TemporaryDirectory(prefix="erase-gpt-", dir=ROOT/".artifacts") as directory:
            for label in (None, "dos", "gpt"):
                image = Path(directory)/(str(label)+".img")
                with image.open("xb") as stream: stream.truncate(24*planner.GIB)
                if label:
                    subprocess.run(["sfdisk", str(image)], input=f'label: {label}\nstart=2048,size=4194304\n',
                                   text=True, capture_output=True, check=True)
                plan = planner.make_erase_plan(str(image), image.stat().st_size, 512)
                before = planner.metadata_fingerprint(image, image.stat().st_size)
                command = ["sfdisk", "--wipe", "always", "--wipe-partitions", "always", str(image)]
                subprocess.run(command[:1]+["--no-act"]+command[1:], input=planner.append_script(plan),
                               text=True, capture_output=True, check=True)
                self.assertEqual(before, planner.metadata_fingerprint(image, image.stat().st_size))
                subprocess.run(command, input=planner.append_script(plan), text=True, capture_output=True, check=True)
                after = json.loads(subprocess.check_output(["sfdisk", "--json", str(image)]))
                self.assertEqual(planner.verify_after(plan, after), [str(image)+"1", str(image)+"2"])

    def test_real_sfdisk_append_preserves_protected_partition_bytes_and_entry_holes(self):
        artifacts = ROOT / ".artifacts"; artifacts.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="coexist-gpt-", dir=artifacts) as directory:
            image = Path(directory)/"disk.img"
            with image.open("wb") as stream: stream.truncate(24*planner.GIB)
            initial = ('label: gpt\n'
                       'start=2048,size=262144,type='+planner.ESP+',name="Windows EFI"\n'
                       'start=264192,size=32768,type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE\n'
                       'start=296960,size=4194304,type='+planner.MICROSOFT+',name="Windows data"\n'
                       'start=4491264,size=1048576,type=DE94BBA4-06D1-4D40-A16A-BFD50179D6AC,name="Recovery"\n')
            subprocess.run(["sfdisk", str(image)], input=initial, text=True, capture_output=True, check=True)
            # Leave entry 2 unused: --append is allowed to reuse an entry hole.
            subprocess.run(["sfdisk", "--delete", str(image), "2"], capture_output=True, check=True)
            before = json.loads(subprocess.check_output(["sfdisk", "--json", str(image)]))
            markers = {}
            with image.open("r+b") as stream:
                for part in before["partitiontable"]["partitions"]:
                    for offset in (part["start"]*512, (part["start"]+part["size"])*512-4096):
                        content = hashlib.sha256(str(offset).encode()).digest()*128
                        stream.seek(offset); stream.write(content); markers[offset] = content
            plan = planner.make_plan(before, image.stat().st_size)
            command = ["sfdisk", "--append", "--lock=yes", "--wipe", "never", "--wipe-partitions", "never", str(image)]
            subprocess.run(command, input=planner.append_script(plan), text=True, capture_output=True, check=True)
            after = json.loads(subprocess.check_output(["sfdisk", "--json", str(image)]))
            nodes = planner.verify_after(plan, after)
            self.assertEqual(len(nodes), 2)
            with image.open("rb") as stream:
                for offset, expected in markers.items():
                    stream.seek(offset); self.assertEqual(stream.read(len(expected)), expected)


if __name__ == "__main__": unittest.main()
