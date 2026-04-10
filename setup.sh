set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

ARTIFACT_DIR="$SCRIPT_DIR/.artifacts"
DISK_IMG="$ARTIFACT_DIR/disk.img"
ROOTFS_PUT_EXE="$SCRIPT_DIR/kernel/zig-out/bin/rootfs_put.exe"
ROOTFS_PUT_BIN="$SCRIPT_DIR/kernel/zig-out/bin/rootfs_put"
PIE_USER_ELF="$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/USERAPP.ELF"
mkdir -p "$ARTIFACT_DIR"

# 1) 古いもの削除
rm -f "$DISK_IMG"
sudo umount /mnt/esp 2>/dev/null || true
sudo losetup -D

# 2) 512MB ディスク作成
dd if=/dev/zero of="$DISK_IMG" bs=1M count=512

# 3) GPT + EFI System パーティション作成
sudo sgdisk -og "$DISK_IMG"
sudo sgdisk -n 1:2048:+192M -t 1:ef00 -c 1:"EFI System" "$DISK_IMG"
sudo sgdisk -n 2:395264:0 -t 2:8300 -c 2:"CapabilityOS Persistent" "$DISK_IMG"

# 4) ループバック接続
sudo losetup -Pf "$DISK_IMG"

# loopデバイス取得
LOOP=$(losetup -j "$DISK_IMG" | cut -d: -f1)
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

# 9) rootfs 初期化
if [ ! -f "$PIE_USER_ELF" ]; then
  echo "missing pie_user ELF: $PIE_USER_ELF" >&2
  exit 1
fi

if [ -f "$ROOTFS_PUT_EXE" ]; then
  if command -v pwsh.exe >/dev/null 2>&1; then
    PS_EXE=pwsh.exe
  else
    PS_EXE=powershell.exe
  fi

  ROOTFS_PUT_WIN=$(wslpath -w "$ROOTFS_PUT_EXE")
  DISK_IMG_WIN=$(wslpath -w "$DISK_IMG")
  PIE_USER_ELF_WIN=$(wslpath -w "$PIE_USER_ELF")
  "$PS_EXE" -NoLogo -NoProfile -Command "& '$ROOTFS_PUT_WIN' '$DISK_IMG_WIN' 2 '/pie_user.elf' '$PIE_USER_ELF_WIN'"
elif [ -x "$ROOTFS_PUT_BIN" ]; then
  "$ROOTFS_PUT_BIN" "$DISK_IMG" 2 /pie_user.elf "$PIE_USER_ELF"
else
  echo "missing rootfs_put tool: $ROOTFS_PUT_EXE" >&2
  exit 1
fi

echo "$DISK_IMG ready."
