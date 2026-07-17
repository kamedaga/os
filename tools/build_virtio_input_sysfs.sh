#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${repo_root}/.artifacts/userland-fixtures/virtio-input-sysfs"
rm -rf "${out}"

raw_device_count="${CAPOS_INPUT_SYSFS_DEVICE_COUNT:-2}"
if [[ ! "${raw_device_count}" =~ ^[0-9]+$ ]] || ((${#raw_device_count} > 3)); then
  echo "CAPOS_INPUT_SYSFS_DEVICE_COUNT must be a decimal integer from 0 through 192" >&2
  exit 2
fi
device_count="$((10#${raw_device_count}))"
if ((device_count > 192)); then
  echo "CAPOS_INPUT_SYSFS_DEVICE_COUNT must not exceed 192" >&2
  exit 2
fi

write_link() {
  local path="$1" target="$2"
  mkdir -p "$(dirname "${path}")"
  printf 'CAPABILITYOS_ROOTFS_SYMLINK\n%s' "${target}" >"${path}"
}

make_device() {
  local event="$1"
  local input="${out}/sys/devices/virtual/input/input${event}"
  local event_dir="${input}/event${event}"
  local minor="$((64 + event))"

  mkdir -p "${event_dir}" "${out}/dev/input" \
    "${out}/sys/class/input" "${out}/sys/bus/input"
  printf '%s\n' 'CAPABILITYOS_ROOTFS_DEVICE' "c 13 ${minor}" \
    >"${out}/dev/input/event${event}"
  printf '13:%s\n' "${minor}" >"${event_dir}/dev"
  printf '%s\n' \
    "MAJOR=13" "MINOR=${minor}" "DEVNAME=input/event${event}" \
    'ID_INPUT=1' >"${event_dir}/uevent"
  write_link "${event_dir}/subsystem" '../../../../../bus/input'
  write_link "${out}/sys/class/input/event${event}" \
    "../../devices/virtual/input/input${event}/event${event}"
  write_link "${out}/sys/dev/char/13:${minor}" \
    "../../devices/virtual/input/input${event}/event${event}"

  mkdir -p "${input}/capabilities" "${input}/id"
  printf 'Pacha Virtual Input %s\n' "${event}" >"${input}/name"
  printf 'pacha/input%s\n' "${event}" >"${input}/phys"
  printf '%s\n' '0' >"${input}/properties"
  printf '%s\n' '1' >"${input}/capabilities/ev"
  printf '%s\n' '0' >"${input}/capabilities/key"
  printf '%s\n' '0' >"${input}/capabilities/rel"
  printf '%s\n' '0' >"${input}/capabilities/abs"
  printf '%s\n' '0' >"${input}/capabilities/msc"
  printf '%s\n' '0' >"${input}/capabilities/sw"
  printf '%s\n' '0006' >"${input}/id/bustype"
  printf '%s\n' '0000' >"${input}/id/vendor"
  printf '%s\n' '0000' >"${input}/id/product"
  printf '%s\n' '0001' >"${input}/id/version"
  printf '%s\n' \
    'PRODUCT=6/0/0/1' \
    "NAME=\"Pacha Virtual Input ${event}\"" \
    "PHYS=\"pacha/input${event}\"" \
    'PROP=0' \
    >"${input}/uevent"
  write_link "${input}/subsystem" '../../../../bus/input'
}

for ((event = 0; event < device_count; event++)); do
  make_device "${event}"
done
