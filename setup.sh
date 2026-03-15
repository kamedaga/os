set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

# 1) 古いもの削除
rm -f disk.img
sudo umount /mnt/esp 2>/dev/null || true
sudo losetup -D

# 2) 128MB ディスク作成
dd if=/dev/zero of=disk.img bs=1M count=128

# 3) GPT + EFI System パーティション作成
sudo sgdisk -og disk.img
sudo sgdisk -n 1:2048:0 -t 1:ef00 -c 1:"EFI System" disk.img

# 4) ループバック接続
sudo losetup -Pf disk.img

# loopデバイス取得
LOOP=$(losetup -j disk.img | cut -d: -f1)
echo "Loop device: $LOOP"

# 5) FATフォーマット
sudo mkfs.vfat ${LOOP}p1

# 6) マウント
sudo mkdir -p /mnt/esp
sudo mount ${LOOP}p1 /mnt/esp

# 7) EFIコピー
sudo mkdir -p /mnt/esp/EFI/BOOT
sudo cp kernel/zig-out/bin/EFI/BOOT/BOOTX64.EFI /mnt/esp/EFI/BOOT/
sudo cp kernel/zig-out/bin/EFI/BOOT/*.ELF /mnt/esp/EFI/BOOT/
if [ -f kernel/zig-out/bin/EFI/BOOT/BOOTFS.IMG ]; then
  sudo cp kernel/zig-out/bin/EFI/BOOT/BOOTFS.IMG /mnt/esp/EFI/BOOT/
fi
if [ -f kernel/zig-out/EFI/BOOT/CAPCHEL.ELF ]; then
  sudo cp kernel/zig-out/EFI/BOOT/CAPCHEL.ELF /mnt/esp/EFI/BOOT/
fi

# 8) 後処理
sudo umount /mnt/esp
sudo losetup -d $LOOP

echo "disk.img ready."
