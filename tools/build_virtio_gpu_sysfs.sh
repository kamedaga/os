#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${repo_root}/.artifacts/userland-fixtures/virtio-gpu-sysfs"
pci="${out}/sys/devices/pci0000:00/0000:00:03.0"
virtio="${pci}/virtio1"

rm -rf "${out}"
card="${virtio}/drm/card0"
mkdir -p "${out}/dev/dri" "${card}" "${out}/sys/dev/char" \
    "${out}/sys/class/drm" "${out}/sys/bus/pci" "${out}/sys/bus/virtio" \
    "${out}/sys/bus/drm"
printf '%s\n' 'CAPABILITYOS_ROOTFS_DEVICE' 'c 226 0' >"${out}/dev/dri/card0"
printf '%s\n' 'CAPABILITYOS_ROOTFS_DEVICE' 'c 10 60' >"${out}/dev/udmabuf"
printf '%s\n' '226:0' >"${card}/dev"
printf '%s\n' 'MAJOR=226' 'MINOR=0' 'DEVNAME=dri/card0' \
    'DEVTYPE=drm_minor' >"${card}/uevent"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' '/sys/bus/drm' >"${card}/subsystem"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' '../..' >"${card}/device"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' \
    '../../devices/pci0000:00/0000:00:03.0/virtio1/drm/card0' \
    >"${out}/sys/dev/char/226:0"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' \
    '../../devices/pci0000:00/0000:00:03.0/virtio1/drm/card0' \
    >"${out}/sys/class/drm/card0"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' '../../../bus/pci' >"${pci}/subsystem"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' '../../../../bus/virtio' >"${virtio}/subsystem"

for path in "${pci}"; do
    printf '%s\n' '0x1af4' >"${path}/vendor"
    printf '%s\n' '0x1050' >"${path}/device"
    printf '%s\n' '0x1af4' >"${path}/subsystem_vendor"
    printf '%s\n' '0x1100' >"${path}/subsystem_device"
    printf '%s\n' '0x01' >"${path}/revision"
    printf '%s\n' \
        'DRIVER=virtio-pci' \
        'PCI_CLASS=038000' \
        'PCI_ID=1AF4:1050' \
        'PCI_SUBSYS_ID=1AF4:1100' \
        'PCI_SLOT_NAME=0000:00:03.0' \
        >"${path}/uevent"
done
printf '%s\n' \
    'DRIVER=virtio_gpu' \
    'MODALIAS=virtio:d00000010v00001AF4' \
    >"${virtio}/uevent"
