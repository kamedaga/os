SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

ARTIFACT_DIR="$SCRIPT_DIR/.artifacts"
DISK_IMG="$ARTIFACT_DIR/disk.img"
OVMF_VARS="$ARTIFACT_DIR/OVMF_VARS.fd"
QEMU_LOG="$ARTIFACT_DIR/qemu.log"
EFI_DIR="$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT"
mkdir -p "$ARTIFACT_DIR"

if [ ! -f "$DISK_IMG" ]; then
  echo "missing $DISK_IMG"
  echo "run ./setup.sh first"
  exit 1
fi

for f in "$EFI_DIR"/BOOTX64.EFI "$EFI_DIR"/*.ELF "$EFI_DIR"/BOOTFS.IMG; do
  [ -e "$f" ] || continue
  if [ "$f" -nt "$DISK_IMG" ]; then
    echo "stale disk image detected:"
    echo "  $f is newer than $DISK_IMG"
    echo "run ./setup.sh to refresh the EFI partition"
    exit 1
  fi
done

rm -f "$OVMF_VARS"
rm -f "$QEMU_LOG"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS"

qemu-system-x86_64 \
  -machine q35 \
  -m 512M \
  -monitor none \
  -d int,guest_errors,cpu_reset \
  -D "$QEMU_LOG" \
  -display gtk,grab-on-hover=off \
  -vga none \
  -device virtio-vga \
  -device virtio-tablet-pci \
  -device virtio-keyboard-pci \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive if=none,file="$DISK_IMG",format=raw,id=bootdisk \
  -device virtio-blk-pci,drive=bootdisk \
  -serial stdio
