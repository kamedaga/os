#!/usr/bin/env bash
set -euo pipefail

kernel_version=${KERNEL_VERSION:-6.8.0-117-generic}
modules_pkg=${KERNEL_MODULES_PACKAGE:-linux-modules-6.8.0-117-generic}
modules_extra_pkg=${KERNEL_MODULES_EXTRA_PACKAGE:-linux-modules-extra-6.8.0-117-generic}
zstd_pkg=${ZSTD_PACKAGE:-zstd}
usb_stack_root=${KOBOX_USB_STACK_MODULES_ROOT:-/lib/modules/$(uname -r)}
debs_dir=.artifacts/debs
work_dir=.artifacts/kobox-modules
out_dir=.artifacts/userland-fixtures/kobox-modules

mkdir -p "$debs_dir" "$work_dir" "$out_dir"

fetch_deb() {
  pkg="$1"
  if ! ls "$debs_dir"/"$pkg"_*.deb >/dev/null 2>&1; then
    (cd "$debs_dir" && apt download "$pkg")
  fi
}

extract_deb_once() {
  pkg="$1"
  stamp="$2"
  dest="$3"
  if [ ! -e "$stamp" ]; then
    rm -rf "$dest"
    mkdir -p "$dest"
    dpkg-deb -x "$debs_dir"/"$pkg"_*.deb "$dest"
    : > "$stamp"
  fi
}

fetch_deb "$modules_pkg"
fetch_deb "$modules_extra_pkg"
fetch_deb "$zstd_pkg"

modules_root="$work_dir/modules-root"
modules_extra_root="$work_dir/modules-extra-root"
zstd_root="$work_dir/zstd-root"
extract_deb_once "$modules_pkg" "$work_dir/.modules-extracted" "$modules_root"
extract_deb_once "$modules_extra_pkg" "$work_dir/.modules-extra-extracted" "$modules_extra_root"
extract_deb_once "$zstd_pkg" "$work_dir/.zstd-extracted" "$zstd_root"
zstd_bin="$zstd_root/usr/bin/zstd"

copy_module() {
  rel="$1"
  out="$2"
  for root in "$modules_root" "$modules_extra_root"; do
    src="$root/lib/modules/$kernel_version/$rel"
    if [ -f "$src" ]; then
      cp "$src" "$out_dir/$out"
      return
    fi
    if [ -f "$src.zst" ]; then
      "$zstd_bin" -q -d -f "$src.zst" -o "$out_dir/$out"
      return
    fi
  done
  echo "missing module: $rel" >&2
  exit 1
}

copy_usb_module() {
  rel="$1"
  out="$2"
  src="$usb_stack_root/$rel"
  if [ -f "$src" ]; then
    cp "$src" "$out_dir/$out"
    return
  fi
  if [ -f "$src.zst" ]; then
    "$zstd_bin" -q -d -f "$src.zst" -o "$out_dir/$out"
    return
  fi
  copy_module "$rel" "$out"
}

copy_module kernel/drivers/nvme/common/nvme-auth.ko nvme-auth.ko
copy_module kernel/drivers/nvme/host/nvme-core.ko nvme-core.ko
copy_module kernel/drivers/nvme/host/nvme.ko nvme.ko

copy_usb_module kernel/drivers/usb/core/usbcore.ko usbcore.ko
copy_usb_module kernel/drivers/usb/host/xhci-hcd.ko xhci-hcd.ko
copy_usb_module kernel/drivers/usb/host/xhci-pci.ko xhci-pci.ko
copy_usb_module kernel/drivers/usb/storage/usb-storage.ko usb-storage.ko
copy_usb_module kernel/drivers/hid/hid.ko hid.ko
copy_usb_module kernel/drivers/hid/hid-generic.ko hid-generic.ko
copy_usb_module kernel/drivers/hid/usbhid/usbhid.ko usbhid.ko
