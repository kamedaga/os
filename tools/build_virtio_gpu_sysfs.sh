#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${repo_root}/.artifacts/userland-fixtures/virtio-gpu-sysfs"
pci="${out}/sys/devices/pci0000:00/0000:00:03.0"
virtio="${pci}/virtio1"

rm -rf "${out}"
sysdev="${out}/sys/dev/char/226:0/device"
mkdir -p "${out}/dev/dri" "${sysdev}/drm/card0" "${virtio}/drm/card0" \
    "${out}/sys/bus/pci" "${out}/sys/bus/virtio"
: >"${out}/dev/dri/card0"
: >"${out}/dev/udmabuf"
printf '%s\n' '226:0' >"${sysdev}/drm/card0/dev"
printf '%s\n' '226:0' >"${virtio}/drm/card0/dev"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' \
    '../../../../bus/virtio' >"${sysdev}/subsystem"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' \
    '../../../bus/pci' >"${out}/sys/dev/char/226:0/subsystem"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' '../../../bus/pci' >"${pci}/subsystem"
printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' '../../../../bus/virtio' >"${virtio}/subsystem"

for path in "${pci}" "${sysdev}"; do
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
