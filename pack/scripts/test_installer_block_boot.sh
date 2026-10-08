#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ $# != 2 ]]; then
    echo "usage: $0 INSTALLER_LIMINE_IMAGE BOOTFS_IMAGE" >&2
    echo 'Both inputs remain unchanged; only temporary QEMU disk images are used.' >&2
    exit 2
fi
image="$(realpath "$1")"
bootfs="$(realpath "$2")"
[[ -f "$image" && -f "$bootfs" ]] || exit 2
mkdir -p "$repo_root/.artifacts"
work="$(mktemp -d "$repo_root/.artifacts/installer-block-boot.XXXXXX")"
echo "Evidence: $work"
sha256sum "$image" "$bootfs" > "$work/inputs.sha256"
cp --sparse=always "$image" "$work/boot.img"
cp --sparse=always "$image" "$work/install-data.img"

# Read the ESP from GPT rather than coupling this test to one image layout.
esp_offset="$(python3 - "$work/boot.img" <<'PY'
import struct
import sys
import uuid

with open(sys.argv[1], 'rb') as stream:
    stream.seek(512)
    header = stream.read(512)
    if header[:8] != b'EFI PART':
        raise SystemExit('input is not a GPT installer image')
    table, count, stride = struct.unpack_from('<QII', header, 72)
    if not 1 <= count <= 4096 or not 128 <= stride <= 4096:
        raise SystemExit('invalid GPT entry layout')
    esp = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
    offsets = []
    for index in range(count):
        stream.seek(table * 512 + index * stride)
        entry = stream.read(stride)
        if len(entry) != stride:
            raise SystemExit('truncated GPT entries')
        if entry[:16] == esp:
            offsets.append(struct.unpack_from('<Q', entry, 32)[0] * 512)
    if len(offsets) != 1:
        raise SystemExit('input must have exactly one ESP')
    print(offsets[0])
PY
)"
mdel -i "$work/boot.img@@$esp_offset" ::/BOOTFS.IMG
mcopy -i "$work/boot.img@@$esp_offset" "$bootfs" ::/BOOTFS.IMG
mcopy -i "$work/boot.img@@$esp_offset" ::/BOOTFS.IMG "$work/bootfs.readback"
cmp "$bootfs" "$work/bootfs.readback"
truncate -s 2G "$work/target.img"
sha256sum "$work/target.img" "$work/install-data.img" > "$work/disks.sha256"

# No host device paths can enter this command. The writable namespace is a
# fresh sparse file so the read-only discovery test also detects stray writes.
set +e
timeout --kill-after=5s 45s go -C "$repo_root/pack" run ./cmd/pacgo qemu-limine \
    --image "$work/boot.img" --firmware uefi --no-storage --no-net \
    --input-profile usb-hid --display none --memory 4G \
    --qemu-arg=-drive \
    --qemu-arg="if=none,file=$work/target.img,format=raw,id=nvmetarget" \
    --qemu-arg=-device --qemu-arg=nvme,drive=nvmetarget,serial=PACHAINSTALLTEST \
    --qemu-arg=-drive \
    --qemu-arg="if=none,file=$work/install-data.img,format=raw,id=installmedia,readonly=on" \
    --qemu-arg=-device --qemu-arg=usb-storage,drive=installmedia,bus=pachaxhci.0 \
    > "$work/serial.log" 2>&1
result=$?
set -e
[[ "$result" == 124 ]] || { echo "Unexpected QEMU exit: $result" >&2; exit 1; }
sha256sum --check "$work/inputs.sha256"
sha256sum --check "$work/disks.sha256"
rg -q '\[usbd\] nvme ready generation=[1-9]' "$work/serial.log"
rg -q '\[usbd\] block nvme0n1 .* bytes=2147483648 logical=512 ro=0 gpt=-22 install_media=0' "$work/serial.log"
rg -q '\[usbd\] block sd[a-z]+ .* ro=1 gpt=0 install_media=1' "$work/serial.log"
if rg -n 'invalid DMA range|AMD IOMMU initialization failed|unit preparation failed' "$work/serial.log"; then
    exit 1
fi
echo 'PASS: NVMe namespace read, USB installer GPT identification, no disk writes'
