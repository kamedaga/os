set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

ARTIFACT_DIR="$SCRIPT_DIR/.artifacts"
DISK_IMG="$ARTIFACT_DIR/disk.img"
ROOTFS_BUILDER_EXE="$SCRIPT_DIR/kernel/zig-out/bin/rootfs_builder.exe"
ROOTFS_BUILDER_BIN="$SCRIPT_DIR/kernel/zig-out/bin/rootfs_builder"
ESP_BUILDER_EXE="$SCRIPT_DIR/kernel/zig-out/bin/esp_builder.exe"
ESP_BUILDER_BIN="$SCRIPT_DIR/kernel/zig-out/bin/esp_builder"
ROOTFS_MANIFEST="$SCRIPT_DIR/userland/rootfs/rootfs_manifest.txt"
SEED_INIT_DIR="$SCRIPT_DIR/userland/seed_init"
SEED_INIT_OUT="$SEED_INIT_DIR/zig-out/bin/seed.elf"
ESP_MANIFEST="$SCRIPT_DIR/bootstrap/esp_manifest.txt"
mkdir -p "$ARTIFACT_DIR"

# 1) 古いもの削除
rm -f "$DISK_IMG"

# 2) 512MB ディスク作成
dd if=/dev/zero of="$DISK_IMG" bs=1M count=512

# 3) GPT + EFI System パーティション作成
sgdisk -og "$DISK_IMG"
sgdisk -n 1:2048:+192M -t 1:ef00 -c 1:"EFI System" "$DISK_IMG"
sgdisk -n 2:395264:0 -t 2:8300 -c 2:"CapabilityOS Persistent" "$DISK_IMG"

# 4) ESP 初期化
if [ ! -f "$ESP_MANIFEST" ]; then
  echo "missing esp manifest: $ESP_MANIFEST" >&2
  exit 1
fi

if [ ! -f "$ROOTFS_MANIFEST" ]; then
  echo "missing rootfs manifest: $ROOTFS_MANIFEST" >&2
  exit 1
fi

if command -v pwsh.exe >/dev/null 2>&1; then
  PS_EXE=pwsh.exe
else
  PS_EXE=powershell.exe
fi

build_seed_init() {
  local seed_init_win
  seed_init_win=$(wslpath -w "$SEED_INIT_DIR")
  "$PS_EXE" -NoLogo -NoProfile -Command "Set-Location '$seed_init_win'; zig build seed-elf -Doptimize=ReleaseSmall"
}

build_seed_init

if [ ! -f "$SEED_INIT_OUT" ]; then
  echo "missing seed init artifact: $SEED_INIT_OUT" >&2
  exit 1
fi

run_disk_builder() {
  local exe_path="$1"
  local bin_path="$2"
  local disk_img="$3"
  local partition_index="$4"
  local manifest_path="$5"
  if [ -f "$exe_path" ]; then
    local exe_win
    exe_win=$(wslpath -w "$exe_path")
    local disk_img_win
    local manifest_win
    disk_img_win=$(wslpath -w "$disk_img")
    manifest_win=$(wslpath -w "$manifest_path")
    "$PS_EXE" -NoLogo -NoProfile -Command "& '$exe_win' '$disk_img_win' '$partition_index' '$manifest_win'"
  elif [ -x "$bin_path" ]; then
    "$bin_path" "$disk_img" "$partition_index" "$manifest_path"
  else
    echo "missing host tool: $exe_path" >&2
    exit 1
  fi
}

run_disk_builder "$ESP_BUILDER_EXE" "$ESP_BUILDER_BIN" "$DISK_IMG" 1 "$ESP_MANIFEST"

# 5) rootfs 初期化
run_disk_builder "$ROOTFS_BUILDER_EXE" "$ROOTFS_BUILDER_BIN" "$DISK_IMG" 2 "$ROOTFS_MANIFEST"

echo "$DISK_IMG ready."
