#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
fixture_dir="$root_dir/.artifacts/userland-fixtures"
work_dir="${CAPABILITYOS_WSL_WORKDIR:-$HOME/.cache/capabilityos}/alpine-go-root"
install_root="$work_dir/install"
output_root="$fixture_dir/alpine-go-root"
apk="$fixture_dir/apk.elf"
repositories="$fixture_dir/apk-repositories"
keys_dir="$fixture_dir/alpine-keys"
stamp="$work_dir/.capabilityos-built"
config_id="alpine-v3.22-go-1.24-rootfs-toolchain-only-v2"

mkdir -p "$fixture_dir" "$work_dir"

bash "$root_dir/tools/build_wsl_apk_tools.sh"
bash "$root_dir/tools/build_wsl_apk_config.sh"
bash "$root_dir/tools/build_wsl_alpine_keys.sh"

if [ -f "$stamp" ] && [ -d "$output_root" ] && [ "$(cat "$stamp" 2>/dev/null || true)" = "$config_id" ]; then
  exit 0
fi

if [ ! -f "$stamp" ] || [ "$(cat "$stamp" 2>/dev/null || true)" != "$config_id" ] || [ ! -d "$install_root/usr/lib/go" ]; then
  rm -rf "$install_root"
  mkdir -p \
    "$install_root/etc/apk" \
    "$install_root/etc/apk/keys" \
    "$install_root/lib/apk/db" \
    "$install_root/var/cache/apk"

  cp "$repositories" "$install_root/etc/apk/repositories"
  cp "$keys_dir"/*.rsa.pub "$install_root/etc/apk/keys/"

  "$apk" \
    --root "$install_root" \
    --initdb \
    --no-progress \
    --no-cache \
    --no-scripts \
    --no-chown \
    add go
fi

rm -rf "$output_root.tmp"
mkdir -p "$output_root.tmp"

python3 - "$install_root" "$output_root.tmp" <<'PY'
import os
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]).resolve()
dst = Path(sys.argv[2]).resolve()

include_roots = {
    "usr/bin/go",
    "usr/bin/gofmt",
    "usr/lib/go",
}

def rel(path: Path) -> str:
    return path.relative_to(src).as_posix()

def wanted(path: Path) -> bool:
    name = rel(path)
    return any(
        name == item
        or name.startswith(item + "/")
        or item.startswith(name + "/")
        for item in include_roots
    )

def resolve_link(path: Path) -> Path:
    target = os.readlink(path)
    if target.startswith("/"):
        return src / target.lstrip("/")
    return (path.parent / target).resolve()

for root, dirnames, filenames in os.walk(src, topdown=True, followlinks=False):
    root_path = Path(root)
    dirnames[:] = [
        name for name in dirnames
        if wanted(root_path / name)
    ]
    for name in filenames:
        item = root_path / name
        if not wanted(item):
            continue
        out = dst / rel(item)
        out.parent.mkdir(parents=True, exist_ok=True)
        if item.is_symlink():
            target_text = os.readlink(item)
            target = resolve_link(item)
            if not target.exists():
                raise SystemExit(f"unresolved symlink: {item} -> {target}")
            out.write_bytes(b"CAPABILITYOS_ROOTFS_SYMLINK\n" + target_text.encode())
        elif item.is_file():
            shutil.copy2(item, out)
PY

rm -rf "$output_root"
mv "$output_root.tmp" "$output_root"
printf '%s\n' "$config_id" > "$stamp"
