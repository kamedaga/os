#!/usr/bin/env python3
"""Fetch the AX210 firmware and the unmodified, signed regulatory database."""
import hashlib
from pathlib import Path
import subprocess
import tarfile

from build_wifi_supplicant import fetch, MIRROR

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / '.artifacts/kobox2-wifi-firmware'
MANIFEST = ROOT / 'kobox2/linux-sandbox/kobox/manifest/profiles/wifi-firmware.sha256'
REF = '20260916'


def main():
    expected = dict(line.split()[::-1] for line in MANIFEST.read_text().splitlines()
        if line and not line.startswith('#'))
    for name in ('iwlwifi-ty-a0-gf-a0-89.ucode', 'iwlwifi-ty-a0-gf-a0.pnvm'):
        fetch(f'https://gitlab.com/kernel-firmware/linux-firmware/-/raw/{REF}/intel/iwlwifi/{name}',
            OUT / name, expected[name])
    fetch(f'https://gitlab.com/kernel-firmware/linux-firmware/-/raw/{REF}/LICENSES/LICENCE.iwlwifi_firmware',
        OUT / 'LICENCE.iwlwifi_firmware',
        '16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5')
    archive = ROOT / '.artifacts/wifi-build/wireless-regdb-2025.02.20-r0.apk'
    fetch(MIRROR + archive.name, archive,
        'e95f6ef13ca7a6fce5b43bedffc4f3677af51999b0ce1c8f65e4a3751dfec2eb')
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            name = Path(member.name).name
            if not member.isfile() or name not in ('regulatory.db', 'regulatory.db.p7s'):
                continue
            content = package.extractfile(member).read()
            assert hashlib.sha256(content).hexdigest() == expected[name], name
            (OUT / name).write_bytes(content)
    for name, digest in expected.items():
        assert hashlib.sha256((OUT / name).read_bytes()).hexdigest() == digest, name
    subprocess.run(['python3', str(ROOT / 'pack/scripts/build_amdgpu_firmware_bundle.py'),
        '--profile', 'wifi', '--manifest', str(MANIFEST), '--firmware-dir', str(OUT),
        '--output', str(ROOT / '.artifacts/kobox2-wifi-production/firmware.bundle')], check=True)
    output = ROOT / '.artifacts/kobox2-wifi-production'
    (output / 'firmware.sha256').write_text(
        hashlib.sha256((output / 'firmware.bundle').read_bytes()).hexdigest() + '\n')


if __name__ == '__main__':
    main()
