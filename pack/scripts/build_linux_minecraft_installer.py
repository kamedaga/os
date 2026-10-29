#!/usr/bin/env python3
"""Add a PachaOS payload to an unchanged SystemRescue Linux Live ISO.

Writes artifacts only. Does not rebuild/copy the developer rootfs or write disks.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid

REPO = Path(__file__).resolve().parents[2]
BASE_SHA256 = "ad4d670b72859d887c7960142a9a9d36a3e50446694a035e254442f65d6e7572"


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssh-authorized-key-file", type=Path, required=True)
    parser.add_argument("--base-iso", type=Path, default=REPO / ".artifacts/linux-minecraft-installer/systemrescue-13.02-amd64.iso")
    parser.add_argument("--payload-dir", type=Path, default=REPO / ".artifacts/minecraft-installer")
    parser.add_argument("--output-dir", type=Path, default=REPO / ".artifacts/linux-minecraft-installer")
    parser.add_argument("--kernel", type=Path,
                        help="Use a verified replacement kernel with the existing payload ABI")
    args = parser.parse_args()
    base, payload, out = args.base_iso.resolve(), args.payload_dir.resolve(), args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    assert digest(base) == BASE_SHA256, "SystemRescue base ISO checksum mismatch"
    source = REPO / "pack/linux-installer"
    overlay = out / "overlay"
    data = overlay / "pacha"
    data.mkdir(parents=True, exist_ok=True)
    (overlay / "autorun").mkdir(exist_ok=True)
    (overlay / "sysrescue.d").mkdir(exist_ok=True)
    (overlay / "boot/grub").mkdir(parents=True, exist_ok=True)
    for name, dest in (("autorun", "autorun/pacha-setup"),
                       ("900-pacha.yaml", "sysrescue.d/900-pacha.yaml"),
                       ("grub-custom.cfg", "boot/grub/custom.cfg"),
                       ("pacha-install", "pacha/pacha-install"),
                       ("pacha-install-plan.py", "pacha/pacha-install-plan.py")):
        shutil.copyfile(source / name, overlay / dest)
        (overlay / dest).chmod(0o755 if name in ("autorun", "pacha-install") else 0o644)
    public_key = args.ssh_authorized_key_file.read_text().strip()
    assert public_key.startswith("ssh-ed25519 ") and "PRIVATE KEY" not in public_key
    (data / "authorized_keys").write_text(public_key + "\n")
    root_uuid = str(uuid.UUID((payload / "root-uuid.txt").read_text().strip()))
    # Use the same validated BootFS parser as the native installer. No duplicate
    # archive parser and no invented ABI/manifest version.
    formatter = REPO / ".artifacts/installer-tools/sbin/pacha-install-progress"
    env = dict(os.environ, LD_LIBRARY_PATH=str(formatter.parent.parent / "usr/lib"))
    actual = subprocess.check_output([str(formatter), "--root-uuid", str(payload / "SYSTEM.BOOTFS")], env=env, text=True).strip()
    assert actual == root_uuid, "Boot archive and filesystem UUID disagree"
    (data / "root-uuid.txt").write_text(root_uuid + "\n")
    (data / "limine.conf").write_text("timeout: 0\nserial: yes\ngraphics: no\n/PachaOS\n    protocol: limine\n    path: boot():/KERNEL.ELF\n    resolution: 1024x768x32\n")
    # Extract only the target UEFI loader; do not copy the native live rootfs.
    subprocess.run(["mcopy", "-o", "-i", str(payload / "media/limine-boot.img") + "@@2097152",
                    "::/EFI/PachaOS/LIMINE.EFI", str(data / "LIMINE.EFI")], check=True)
    files = {"ROOTFS.TAR": payload / "ROOTFS.TAR", "SYSTEM.BOOTFS": payload / "SYSTEM.BOOTFS",
             "KERNEL.ELF": args.kernel.resolve(strict=True) if args.kernel else payload / "media/KERNEL.ELF", "INITAPP.ELF": payload / "media/INITAPP.ELF",
             "LIMINE.EFI": data / "LIMINE.EFI", "limine.conf": data / "limine.conf"}
    metadata = {"root_uuid": root_uuid, "files": {name: path.stat().st_size for name, path in files.items()}}
    assert all(size > 0 for size in metadata["files"].values())
    (data / "files.json").write_text(json.dumps(metadata, indent=2) + "\n")
    temporary = out / "pachaos-linux-installer.iso.part"
    temporary.unlink(missing_ok=True)
    command = ["xorriso", "-indev", str(base), "-outdev", str(temporary),
               "-boot_image", "any", "replay", "-map", str(overlay), "/"]
    for name in ("ROOTFS.TAR", "SYSTEM.BOOTFS", "KERNEL.ELF", "INITAPP.ELF"):
        command += ["-map", str(files[name]), "/pacha/" + name]
    subprocess.run(command, check=True)
    result = out / "pachaos-linux-installer.iso"
    temporary.replace(result)
    sha256 = digest(result)
    result.with_suffix(".iso.sha256").write_text(f"{sha256}  {result.name}\n")
    (out / "build.json").write_text(json.dumps({"base_sha256": BASE_SHA256, "image": str(result),
        "bytes": result.stat().st_size, "sha256": sha256, **metadata}, indent=2) + "\n")
    print(f"Ready: {result}\nSHA256: {sha256}")


if __name__ == "__main__":
    main()
