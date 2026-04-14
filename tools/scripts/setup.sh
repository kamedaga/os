set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

ARTIFACT_DIR="$SCRIPT_DIR/.artifacts"
DISK_IMG="$ARTIFACT_DIR/disk.img"
ROOTFS_BUILDER_EXE="$SCRIPT_DIR/kernel/zig-out/bin/rootfs_builder.exe"
ROOTFS_BUILDER_BIN="$SCRIPT_DIR/kernel/zig-out/bin/rootfs_builder"
ESP_BUILDER_EXE="$SCRIPT_DIR/kernel/zig-out/bin/esp_builder.exe"
ESP_BUILDER_BIN="$SCRIPT_DIR/kernel/zig-out/bin/esp_builder"
BOOTFS_BUILDER_EXE="$SCRIPT_DIR/kernel/zig-out/bin/bootfs_builder.exe"
BOOTFS_BUILDER_BIN="$SCRIPT_DIR/kernel/zig-out/bin/bootfs_builder"
BOOTFS_IMAGE_OUT="$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/BOOTFS.IMG"
ROOTFS_MANIFEST="$SCRIPT_DIR/userland/rootfs/rootfs_manifest.txt"
SEED_INIT_DIR="$SCRIPT_DIR/userland/seed_init"
SEED_INIT_OUT="$SEED_INIT_DIR/zig-out/bin/seed.elf"
SHELL_BOOTFS_SRC="$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/SHELL.ELF"
BLOCK_BOOTFS_SRC="$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/VBLKDRV.ELF"
PERSISTENT_FS_BOOTFS_SRC="$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/PERSFS.ELF"
STARTUP_MANIFEST_BOOTFS_SRC="$SCRIPT_DIR/userland/rootfs/startup_manifest.txt"
ESP_MANIFEST="$SCRIPT_DIR/bootstrap/esp_manifest.txt"
mkdir -p "$ARTIFACT_DIR"

usage() {
  cat <<'EOF'
usage: ./setup.sh [--refresh-rootfs] [--fresh]

  --refresh-rootfs  Rebuild the persistent/rootfs partition from the manifest.
  --fresh           Recreate the whole disk image and rebuild rootfs.
EOF
}

REFRESH_ROOTFS=0
RECREATE_DISK=0

while [ $# -gt 0 ]; do
  case "$1" in
    --refresh-rootfs)
      REFRESH_ROOTFS=1
      ;;
    --fresh)
      RECREATE_DISK=1
      REFRESH_ROOTFS=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

require_nonempty_file() {
  local path="$1"
  local label="$2"
  if [ ! -f "$path" ]; then
    echo "missing $label: $path" >&2
    exit 1
  fi
  if [ ! -s "$path" ]; then
    echo "empty $label: $path" >&2
    echo "rebuild the artifact before running ./setup.sh" >&2
    exit 1
  fi
}

repair_bootx64_efi_if_needed() {
  local target="$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/BOOTX64.EFI"
  if [ -s "$target" ]; then
    return 0
  fi
  local cache_dir="$SCRIPT_DIR/kernel/.zig-cache/o"
  if [ ! -d "$cache_dir" ]; then
    return 1
  fi
  local candidate
  candidate=$(find "$cache_dir" -type f -name 'BOOTX64.efi' -size +0c -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1 | cut -d' ' -f2-)
  if [ -z "$candidate" ]; then
    return 1
  fi
  mkdir -p "$(dirname "$target")"
  cp "$candidate" "$target"
  if [ -s "$target" ]; then
    echo "restored EFI boot image from cache: $candidate"
    return 0
  fi
  return 1
}

create_disk_image() {
  rm -f "$DISK_IMG"
  dd if=/dev/zero of="$DISK_IMG" bs=1M count=512
  sgdisk -og "$DISK_IMG"
  sgdisk -n 1:2048:+192M -t 1:ef00 -c 1:"EFI System" "$DISK_IMG"
  sgdisk -n 2:395264:0 -t 2:8300 -c 2:"CapabilityOS Persistent" "$DISK_IMG"
}

if [ ! -f "$DISK_IMG" ]; then
  RECREATE_DISK=1
  REFRESH_ROOTFS=1
fi

if [ "$RECREATE_DISK" -eq 1 ]; then
  echo "creating fresh disk image: $DISK_IMG"
  create_disk_image
else
  echo "preserving existing disk image: $DISK_IMG"
fi

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

build_bootfs_image() {
  local out_path="$1"
  shift
  if [ $(( $# % 2 )) -ne 0 ]; then
    echo "bootfs image pairs must be <image_path> <source_path>" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$out_path")"
  if [ -f "$BOOTFS_BUILDER_EXE" ]; then
    local exe_win
    exe_win=$(wslpath -w "$BOOTFS_BUILDER_EXE")
    local out_win
    out_win=$(wslpath -w "$out_path")
    local ps_cmd
    ps_cmd="& '$exe_win' '$out_win'"
    while [ $# -gt 0 ]; do
      local image_path="$1"
      local source_path="$2"
      shift 2
      local source_win
      source_win=$(wslpath -w "$source_path")
      ps_cmd="$ps_cmd '$image_path' '$source_win'"
    done
    "$PS_EXE" -NoLogo -NoProfile -Command "$ps_cmd"
  elif [ -x "$BOOTFS_BUILDER_BIN" ]; then
    "$BOOTFS_BUILDER_BIN" "$out_path" "$@"
  else
    local script_dir_win
    script_dir_win=$(wslpath -w "$SCRIPT_DIR")
    local out_win
    out_win=$(wslpath -w "$out_path")
    local ps_cmd
    ps_cmd="Set-Location '$script_dir_win'; zig run .\\tools\\bootfs_builder.zig -- '$out_win'"
    while [ $# -gt 0 ]; do
      local image_path="$1"
      local source_path="$2"
      shift 2
      local source_win
      source_win=$(wslpath -w "$source_path")
      ps_cmd="$ps_cmd '$image_path' '$source_win'"
    done
    "$PS_EXE" -NoLogo -NoProfile -Command "$ps_cmd"
  fi
}

if [ ! -f "$SEED_INIT_OUT" ]; then
  echo "missing seed init artifact: $SEED_INIT_OUT" >&2
  exit 1
fi

build_bootfs_image "$BOOTFS_IMAGE_OUT" \
  /srv/seed.elf "$SEED_INIT_OUT" \
  /cmd/shell.elf "$SHELL_BOOTFS_SRC" \
  /srv/virtio_blk.elf "$BLOCK_BOOTFS_SRC" \
  /srv/persistent_fs.elf "$PERSISTENT_FS_BOOTFS_SRC" \
  /sys/startup_manifest.txt "$STARTUP_MANIFEST_BOOTFS_SRC"

repair_bootx64_efi_if_needed || true
require_nonempty_file "$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/BOOTX64.EFI" "EFI boot image"
require_nonempty_file "$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/SHELL.ELF" "shell image"
require_nonempty_file "$SCRIPT_DIR/kernel/zig-out/bin/EFI/BOOT/INITAPP.ELF" "init image"
require_nonempty_file "$BOOTFS_IMAGE_OUT" "bootfs image"

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

if [ "$REFRESH_ROOTFS" -eq 1 ]; then
  echo "refreshing persistent/rootfs partition from manifest"
  run_disk_builder "$ROOTFS_BUILDER_EXE" "$ROOTFS_BUILDER_BIN" "$DISK_IMG" 2 "$ROOTFS_MANIFEST"
else
  echo "preserving persistent/rootfs partition (pass --refresh-rootfs or --fresh to rebuild it)"
fi

echo "$DISK_IMG ready."
