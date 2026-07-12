#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${repo_root}/.artifacts/userland-fixtures/virtio-input-sysfs"
rm -rf "${out}"

write_link() {
  local path="$1" target="$2"
  mkdir -p "$(dirname "${path}")"
  printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' "${target}" >"${path}"
}

make_device() {
  local event="$1" bdf="$2" virtio_index="$3" input_index="$4"
  local name="$5" product="$6" ev="$7" key="$8" rel="$9"
  local pci="${out}/sys/devices/pci0000:00/0000:00:${bdf}.0"
  local virtio="${pci}/virtio${virtio_index}"
  local input="${virtio}/input/input${input_index}"
  local event_dir="${input}/event${event}"
  local minor="$((64 + event))"

  mkdir -p "${event_dir}" "${out}/dev/input" \
    "${out}/sys/class/input" "${out}/sys/bus/pci" \
    "${out}/sys/bus/virtio" "${out}/sys/bus/input"
  printf '%s\n' 'CAPABILITYOS_ROOTFS_DEVICE' "c 13 ${minor}" \
    >"${out}/dev/input/event${event}"
  printf '13:%s\n' "${minor}" >"${event_dir}/dev"
  if [[ "${event}" == "0" ]]; then
    printf '%s\n' \
      "MAJOR=13" "MINOR=${minor}" "DEVNAME=input/event${event}" \
      'ID_INPUT=1' 'ID_INPUT_KEYBOARD=1' >"${event_dir}/uevent"
  else
    printf '%s\n' \
      "MAJOR=13" "MINOR=${minor}" "DEVNAME=input/event${event}" \
      'ID_INPUT=1' 'ID_INPUT_MOUSE=1' >"${event_dir}/uevent"
  fi
  write_link "${event_dir}/subsystem" '../../../../../../../../bus/input'
  write_link "${out}/sys/class/input/event${event}" \
    "../../devices/pci0000:00/0000:00:${bdf}.0/virtio${virtio_index}/input/input${input_index}/event${event}"
  write_link "${out}/sys/dev/char/13:${minor}" \
    "../../devices/pci0000:00/0000:00:${bdf}.0/virtio${virtio_index}/input/input${input_index}/event${event}"

  mkdir -p "${input}/capabilities" "${input}/id"
  printf '%s\n' "${name}" >"${input}/name"
  printf 'virtio%s/input0\n' "${event}" >"${input}/phys"
  printf '%s\n' '0' >"${input}/properties"
  printf '%s\n' "${ev}" >"${input}/capabilities/ev"
  printf '%s\n' "${key}" >"${input}/capabilities/key"
  printf '%s\n' "${rel}" >"${input}/capabilities/rel"
  printf '%s\n' '0' >"${input}/capabilities/abs"
  printf '%s\n' '0' >"${input}/capabilities/msc"
  printf '%s\n' '0' >"${input}/capabilities/sw"
  printf '%s\n' '0006' >"${input}/id/bustype"
  printf '%s\n' '0627' >"${input}/id/vendor"
  printf '%04x\n' "${product}" >"${input}/id/product"
  printf '%04x\n' "${product}" >"${input}/id/version"
  printf '%s\n' \
    "PRODUCT=6/627/${product}/${product}" \
    "NAME=\"${name}\"" \
    "PHYS=\"virtio${event}/input0\"" \
    'PROP=0' \
    >"${input}/uevent"
  write_link "${input}/subsystem" '../../../../../../../bus/input'

  printf '%s\n' '0x1af4' >"${pci}/vendor"
  printf '%s\n' '0x1052' >"${pci}/device"
  printf '%s\n' '0x1af4' >"${pci}/subsystem_vendor"
  printf '%s\n' '0x1100' >"${pci}/subsystem_device"
  printf '%s\n' '0x01' >"${pci}/revision"
  printf '%s\n' \
    'DRIVER=virtio-pci' 'PCI_CLASS=090200' 'PCI_ID=1AF4:1052' \
    'PCI_SUBSYS_ID=1AF4:1100' "PCI_SLOT_NAME=0000:00:${bdf}.0" \
    >"${pci}/uevent"
  write_link "${pci}/subsystem" '../../../bus/pci'
  printf '%s\n' 'DRIVER=virtio_input' 'MODALIAS=virtio:d00000012v00001AF4' >"${virtio}/uevent"
  write_link "${virtio}/subsystem" '../../../../bus/virtio'
}

# pacgo's permanent Q35 order places keyboard and mouse at 00:04.0/00:05.0.
make_device 0 04 2 0 'QEMU Virtio Keyboard' 1 '120003' \
  '7fffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff' '0'
make_device 1 05 3 1 'QEMU Virtio Mouse' 2 '7' \
  '0 0 0 0 0 10000' '3'
