SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

ARTIFACT_DIR="$SCRIPT_DIR/.artifacts"
DISK_IMG="$ARTIFACT_DIR/disk.img"
OVMF_VARS="$ARTIFACT_DIR/OVMF_VARS.fd"
QEMU_LOG="$ARTIFACT_DIR/qemu.log"
mkdir -p "$ARTIFACT_DIR"

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
  -drive file="$DISK_IMG",format=raw \
  -serial stdio
