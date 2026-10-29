#!/usr/bin/env python3
"""Exercise coexistence on a newly created file-backed NVMe, never host disks."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--ssh-key", type=Path, required=True)
    parser.add_argument("--port", type=int, default=2295)
    parser.add_argument("--erase-disk", action="store_true")
    args = parser.parse_args()
    iso, key = args.iso.resolve(strict=True), args.ssh_key.resolve(strict=True)
    output = Path(tempfile.mkdtemp(prefix="coexist-qemu-", dir=ROOT/".artifacts"))
    print("Evidence:", output, flush=True)
    disk = output/"test-nvme.img"
    with disk.open("xb") as stream:
        stream.truncate(24 << 30)
    initial = ("label: gpt\n"
               "start=2048,size=262144,type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B,name=WindowsEFI\n"
               "start=264192,size=32768,type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE,name=MSR\n"
               "start=296960,size=4194304,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,name=Windows\n"
               "start=4491264,size=1048576,type=DE94BBA4-06D1-4D40-A16A-BFD50179D6AC,name=Recovery\n")
    subprocess.run(["sfdisk", str(disk)], input=initial, text=True, check=True,
                   stdout=subprocess.DEVNULL)
    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", output/"vars.fd")
    ssh = ["ssh", "-i", str(key), "-p", str(args.port), "-o", "BatchMode=yes",
           "-o", "IdentitiesOnly=yes", "-o", "ConnectTimeout=3",
           "-o", "StrictHostKeyChecking=accept-new", "-o", "LogLevel=ERROR",
           "-o", "UserKnownHostsFile="+str(output/"known-hosts"), "root@127.0.0.1"]
    process = None
    handles = []

    def start(live):
        nonlocal process
        tag = "linux" if live else "installed"
        log = (output/(tag+"-qemu.log")).open("wb"); handles.append(log)
        command = ["/usr/bin/qemu-system-x86_64", "-machine",
                   "q35,accel=kvm,smm=off,kernel-irqchip=split", "-cpu", "host",
                   "-smp", "4", "-m", "6G", "-drive",
                   "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                   "-drive", "if=pflash,format=raw,file="+str(output/"vars.fd"),
                   "-device", "amd-iommu,intremap=on,device-iotlb=on",
                   "-device", "qemu-xhci,id=usb", "-device", "usb-kbd,bus=usb.0",
                   "-drive", "if=none,id=target,format=raw,file="+str(disk),
                   "-device", "nvme,drive=target,serial=PACHACOEXISTTEST",
                   "-netdev", f"user,id=net,hostfwd=tcp:127.0.0.1:{args.port}-:22",
                   "-device", "virtio-net-pci,netdev=net,disable-legacy=on,iommu_platform=on",
                   "-display", "none", "-serial", "file:"+str(output/(tag+"-serial.log")),
                   "-qmp", "unix:"+str(output/(tag+"-qmp.sock"))+",server=on,wait=off", "-no-reboot"]
        if live:
            command += ["-drive", "if=none,id=media,format=raw,readonly=on,file="+str(iso),
                        "-device", "usb-storage,drive=media,bus=usb.0,bootindex=1"]
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        print("Booting", tag, flush=True)

    def await_ssh(marker, timeout=240):
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before SSH was ready")
            result = subprocess.run(ssh+[marker], capture_output=True, text=True)
            if result.returncode == 0:
                print("SSH ready", flush=True)
                return
            time.sleep(5)
        raise RuntimeError("SSH readiness timed out")

    def remote(script, name, timeout=360):
        with (output/(name+".log")).open("w") as log:
            result = subprocess.run(ssh+["bash -se"], input=script, text=True,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        if result.returncode:
            raise RuntimeError(name+" failed; see "+str(output/(name+".log")))
        print(name, "PASS", flush=True)

    try:
        start(True)
        await_ssh("test -x /usr/local/sbin/pacha-install")
        preservation = r'''
test "$(lsblk -dnro SERIAL /dev/nvme0n1)" = PACHACOEXISTTEST
mkfs.fat -F32 -n WINDOWS_EFI /dev/nvme0n1p1
mkfs.ntfs -F -Q -L Windows /dev/nvme0n1p3
mkfs.ntfs -F -Q -L Recovery /dev/nvme0n1p4
mkdir -p /mnt/windows-efi
mount -t vfat /dev/nvme0n1p1 /mnt/windows-efi
mkdir -p /mnt/windows-efi/EFI/Microsoft/Boot /mnt/windows-efi/EFI/BOOT
printf 'WINDOWS_BOOT_MANAGER_SENTINEL\n' >/mnt/windows-efi/EFI/Microsoft/Boot/bootmgfw.efi
printf 'WINDOWS_FALLBACK_SENTINEL\n' >/mnt/windows-efi/EFI/BOOT/BOOTX64.EFI
umount /mnt/windows-efi
efibootmgr --create --disk /dev/nvme0n1 --part 1 --label 'Windows Boot Manager' --loader '\EFI\Microsoft\Boot\bootmgfw.efi'
efibootmgr -v >/run/coexist-uefi.before
sfdisk --json /dev/nvme0n1 >/run/coexist-gpt.before
sha256sum /dev/nvme0n1p1 /dev/nvme0n1p2 /dev/nvme0n1p3 /dev/nvme0n1p4 >/run/coexist-protected.sha256
pacha-install --plan /dev/nvme0n1
sfdisk --json /dev/nvme0n1 >/run/coexist-gpt.after-plan
cmp /run/coexist-gpt.before /run/coexist-gpt.after-plan
if printf 'cancel\n' | pacha-install /dev/nvme0n1; then exit 1; fi
sfdisk --json /dev/nvme0n1 >/run/coexist-gpt.after-cancel
cmp /run/coexist-gpt.before /run/coexist-gpt.after-cancel
sha256sum -c /run/coexist-protected.sha256
'''
        if args.erase_disk:
            preservation = preservation.replace("pacha-install", "pacha-install --erase-disk")
            # The coexistence confirmation must never authorize whole-disk erase.
            preservation += r'''
if printf 'ADD PACHAOS /dev/nvme0n1\n' | pacha-install --erase-disk /dev/nvme0n1; then exit 1; fi
sfdisk --json /dev/nvme0n1 >/run/coexist-gpt.after-wrong-confirmation
cmp /run/coexist-gpt.before /run/coexist-gpt.after-wrong-confirmation
sha256sum -c /run/coexist-protected.sha256
'''
        remote(preservation, "plan-and-cancel-preservation")
        install = r'''
printf 'ADD PACHAOS /dev/nvme0n1\n' | pacha-install /dev/nvme0n1
sha256sum -c /run/coexist-protected.sha256
efibootmgr -v >/run/coexist-uefi.after
old=$(sed -n 's/^BootOrder: //p' /run/coexist-uefi.before)
new=$(sed -n 's/^BootOrder: //p' /run/coexist-uefi.after)
test "${new%,*}" = "$old"
test "$(lsblk -nr -o PARTLABEL /dev/nvme0n1 | grep -c '^PachaOS')" = 2
mkdir -p /mnt/windows-efi
mount -t vfat -o ro /dev/nvme0n1p1 /mnt/windows-efi
grep -qx WINDOWS_BOOT_MANAGER_SENTINEL /mnt/windows-efi/EFI/Microsoft/Boot/bootmgfw.efi
grep -qx WINDOWS_FALLBACK_SENTINEL /mnt/windows-efi/EFI/BOOT/BOOTX64.EFI
umount /mnt/windows-efi
# One-shot boot for this test only; the installer preserved Windows' default.
efibootmgr --bootnext "${new##*,}"
'''
        if args.erase_disk:
            install = r'''
printf 'ERASE DISK /dev/nvme0n1\n' | pacha-install --erase-disk /dev/nvme0n1
sfdisk --json /dev/nvme0n1 >/run/erase-gpt.after
python3 - <<'PY'
import json
old = json.load(open('/run/coexist-gpt.before'))['partitiontable']
new = json.load(open('/run/erase-gpt.after'))['partitiontable']
assert old['id'] != new['id'] and len(new['partitions']) == 2
assert [p['name'] for p in new['partitions']] == ['PachaOS EFI', 'PachaOS root']
assert not {p['uuid'] for p in old['partitions']} & {p['uuid'] for p in new['partitions']}
assert new['partitions'][1]['size'] * new['sectorsize'] > 23 * 1024**3
PY
efibootmgr -v >/run/erase-uefi.after
old=$(sed -n 's/^BootOrder: //p' /run/coexist-uefi.before)
new=$(sed -n 's/^BootOrder: //p' /run/erase-uefi.after)
test "${new#*,}" = "$old"
efibootmgr -v | grep "^Boot${new%%,*}" | grep -q 'PachaOS'
'''
        remote(install, "erase-installation" if args.erase_disk else "installation-and-windows-preservation")
        protected = subprocess.check_output(ssh+["cat /run/coexist-protected.sha256"], text=True)
        (output/"protected.sha256").write_text(protected)
        subprocess.run(ssh+["poweroff"], capture_output=True)
        process.wait(timeout=45)
        # Installed Dropbear generates its own host key. Keep the Linux key
        # evidence and use a separate known_hosts file for the new OS.
        ssh[ssh.index("UserKnownHostsFile="+str(output/"known-hosts"))] = "UserKnownHostsFile="+str(output/"installed-known-hosts")
        start(False)
        await_ssh("test -f /etc/pacha-release", timeout=180)
        result = subprocess.run(ssh+["cat /etc/pacha-release; ls -l /usr/bin/xfce4-session /usr/bin/epiphany; /bin/busybox sync"],
                                capture_output=True, text=True, check=True)
        (output/"installed-root.log").write_text(result.stdout+result.stderr)
        print("Installed PachaOS SSH without USB PASS", flush=True)
        # This test concerns installation and preserved data, not the guest's
        # ACPI power-off implementation. Sync above, then close our own VM.
        with socket.socket(socket.AF_UNIX) as connection:
            connection.connect(str(output/"installed-qmp.sock"))
            stream = connection.makefile("rwb", buffering=0)
            stream.readline()
            for command in ("qmp_capabilities", "quit"):
                stream.write(json.dumps({"execute": command}).encode()+b"\n")
                while True:
                    line = stream.readline()
                    if not line and command == "quit": break
                    reply = json.loads(line)
                    if "error" in reply: raise RuntimeError(reply)
                    if "return" in reply: break
        process.wait(timeout=15)
        before = json.loads(subprocess.check_output(["sfdisk", "--json", str(disk)]))
        parts = before["partitiontable"]["partitions"]
        with disk.open("rb") as stream:
            for line in ([] if args.erase_disk else protected.splitlines()):
                expected, node = line.split()
                number = int(node.rsplit("p", 1)[1])
                part = next(p for p in parts if p["node"] == str(disk)+str(number))
                stream.seek(part["start"]*512); remaining = part["size"]*512
                digest = hashlib.sha256()
                while remaining:
                    data = stream.read(min(remaining, 4 << 20)); assert data
                    digest.update(data); remaining -= len(data)
                assert digest.hexdigest() == expected, node
        result = {"mode": "erase-disk" if args.erase_disk else "coexist",
                  "plan_no_writes": True, "cancel_no_writes": True,
                  "existing_partition_full_hashes_unchanged": not args.erase_disk,
                  "windows_efi_and_fallback_unchanged": not args.erase_disk,
                  "windows_default_boot_order_preserved": not args.erase_disk,
                  "erase_replaces_all_partitions": args.erase_disk,
                  "erase_rejects_coexist_confirmation": args.erase_disk,
                  "pachaos_default_boot": args.erase_disk,
                  "installed_pachaos_boot_without_usb": True,
                  "installed_ssh": True, "real_windows_boot_test": False,
                  "acpi_poweroff_validation": False,
                  "test_disk": str(disk), "iso": str(iso)}
        (output/"result.json").write_text(json.dumps(result, indent=2)+"\n")
        print(json.dumps(result, indent=2), flush=True)
    finally:
        if process and process.poll() is None:
            process.terminate()
            try: process.wait(timeout=15)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        for handle in handles: handle.close()


if __name__ == "__main__": main()
