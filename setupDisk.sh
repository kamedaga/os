set -e

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

# 8) 後処理
sudo umount /mnt/esp
sudo losetup -d $LOOP

echo "disk.img ready."