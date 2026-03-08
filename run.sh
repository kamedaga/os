rm -f OVMF_VARS.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd

qemu-system-x86_64 \
  -machine q35 \
  -m 512M \
  -display gtk,grab-on-hover=off \
  -vga none \
  -device virtio-vga \
  -device virtio-tablet-pci \
  -device virtio-keyboard-pci \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -drive file=disk.img,format=raw \
  -serial stdio
