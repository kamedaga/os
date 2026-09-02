#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${repo_root}/.artifacts/userland-fixtures/virtio-input-sysfs"
rm -rf "${out}"

raw_device_count="${CAPOS_INPUT_SYSFS_DEVICE_COUNT:-2}"
if [[ ! "${raw_device_count}" =~ ^[0-9]+$ ]] || ((${#raw_device_count} > 3)); then
  echo "CAPOS_INPUT_SYSFS_DEVICE_COUNT must be a decimal integer from 0 through 2" >&2
  exit 2
fi
device_count="$((10#${raw_device_count}))"
if ((device_count > 2)); then
  echo "CAPOS_INPUT_SYSFS_DEVICE_COUNT must not exceed the two configured virtio-input devices" >&2
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
  local name phys product version product_line ev_bits key_bits rel_bits abs_bits led_bits

  case "${event}" in
    0)
      # QEMU 8.2 virtio-keyboard: QEMU's qcode-to-linux map plus EV_REP
      # and the three keyboard LEDs. Linux input_register_device adds EV_SYN.
      name='QEMU Virtio Keyboard'
      phys='virtio0/input0'
      product='0001'
      version='0001'
      ev_bits='120003'
      key_bits='400000007 ff803078f800dfff febeffff7bcfffff fffffffffffffffe'
      rel_bits='0'
      abs_bits='0'
      led_bits='7'
      ;;
    1)
      # QEMU 8.2 virtio-mouse with wheel-axis=on: X/Y/WHEEL and the
      # button map from hw/input/virtio-input-hid.c.
      name='QEMU Virtio Mouse'
      phys='virtio1/input0'
      product='0002'
      version='0002'
      ev_bits='7'
      key_bits='30400 1f0000 0 0 0 0'
      rel_bits='103'
      abs_bits='0'
      led_bits='0'
      ;;
    *)
      echo "missing virtio-input capability profile for event${event}" >&2
      return 2
      ;;
  esac
  printf -v product_line 'PRODUCT=6/627/%x/%x' \
    "$((16#${product}))" "$((16#${version}))"

  mkdir -p "${event_dir}" "${out}/dev/input" \
    "${out}/sys/class/input" "${out}/sys/bus/input"
  printf '%s\n' 'CAPABILITYOS_ROOTFS_DEVICE' "c 13 ${minor}" \
    >"${out}/dev/input/event${event}"
  printf '13:%s\n' "${minor}" >"${event_dir}/dev"
  printf '%s\n' \
    "MAJOR=13" "MINOR=${minor}" "DEVNAME=input/event${event}" \
    >"${event_dir}/uevent"
  write_link "${event_dir}/subsystem" '../../../../../bus/input'
  write_link "${out}/sys/class/input/event${event}" \
    "../../devices/virtual/input/input${event}/event${event}"
  write_link "${out}/sys/dev/char/13:${minor}" \
    "../../devices/virtual/input/input${event}/event${event}"

  mkdir -p "${input}/capabilities" "${input}/id"
  printf '%s\n' "${name}" >"${input}/name"
  printf '%s\n' "${phys}" >"${input}/phys"
  printf '%s\n' '0' >"${input}/properties"
  printf '%s\n' "${ev_bits}" >"${input}/capabilities/ev"
  printf '%s\n' "${key_bits}" >"${input}/capabilities/key"
  printf '%s\n' "${rel_bits}" >"${input}/capabilities/rel"
  printf '%s\n' "${abs_bits}" >"${input}/capabilities/abs"
  printf '%s\n' '0' >"${input}/capabilities/msc"
  printf '%s\n' "${led_bits}" >"${input}/capabilities/led"
  printf '%s\n' '0' >"${input}/capabilities/snd"
  printf '%s\n' '0' >"${input}/capabilities/ff"
  printf '%s\n' '0' >"${input}/capabilities/sw"
  printf '%s\n' '0006' >"${input}/id/bustype"
  printf '%s\n' '0627' >"${input}/id/vendor"
  printf '%s\n' "${product}" >"${input}/id/product"
  printf '%s\n' "${version}" >"${input}/id/version"
  printf '%s\n' \
    "${product_line}" \
    "NAME=\"${name}\"" \
    "PHYS=\"${phys}\"" \
    'PROP=0' \
    >"${input}/uevent"
  write_link "${input}/subsystem" '../../../../bus/input'

  # libudev-zero consumes properties from the event uevent file. Generate
  # those properties from the sysfs capability bitmap with udev input_id's
  # rules; the session that happens to start Xorg is not part of this path.
  python3 "${repo_root}/tools/derive_input_id.py" "${input}" >>"${event_dir}/uevent"
}

for ((event = 0; event < device_count; event++)); do
  make_device "${event}"
done
