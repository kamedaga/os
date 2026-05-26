#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
fixture_dir="$root_dir/.artifacts/userland-fixtures"
work_dir="$root_dir/.artifacts/src/alpine-clang-root"
install_root="$work_dir/install"
output_root="$fixture_dir/alpine-clang-root"
apk="$fixture_dir/apk.elf"
repositories="$fixture_dir/apk-repositories"
keys_dir="$fixture_dir/alpine-keys"
stamp="$work_dir/.capabilityos-built"
config_id="alpine-v3.22-clang-host-apk-v14"

mkdir -p "$fixture_dir" "$work_dir"

bash "$root_dir/tools/build_wsl_apk_tools.sh"
bash "$root_dir/tools/build_wsl_apk_config.sh"
bash "$root_dir/tools/build_wsl_alpine_keys.sh"

if [ -f "$stamp" ] && [ -d "$output_root" ] && [ "$(cat "$stamp" 2>/dev/null || true)" = "$config_id" ]; then
  exit 0
fi

rm -rf "$install_root" "$output_root.tmp"
mkdir -p \
  "$install_root/etc/apk" \
  "$install_root/etc/apk/keys" \
  "$install_root/lib/apk/db" \
  "$install_root/var/cache/apk" \
  "$output_root.tmp"

cp "$repositories" "$install_root/etc/apk/repositories"
cp "$keys_dir"/*.rsa.pub "$install_root/etc/apk/keys/"

"$apk" \
  --root "$install_root" \
  --initdb \
  --no-progress \
  --no-cache \
  --no-scripts \
  --no-chown \
  add clang

python3 - "$install_root" "$output_root.tmp" <<'PY'
import os
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]).resolve()
dst = Path(sys.argv[2]).resolve()

exclude = {
    "etc/apk/arch",
    "etc/apk/repositories",
    "etc/apk/world",
    "lib/ld-musl-x86_64.so.1",
    "usr/lib/libLLVM-20.so",
}
exclude_dirs = {
    "etc/apk/keys",
}
exclude_prefixes = (
    "usr/include/",
    "usr/lib/cmake/",
    "usr/lib/gcc/x86_64-alpine-linux-musl/14.2.0/include/",
    "usr/lib/gcc/x86_64-alpine-linux-musl/14.2.0/plugin/include/",
    "usr/lib/llvm20/include/",
    "usr/lib/llvm20/lib/clang/20/include/",
    "usr/share/",
    "usr/x86_64-alpine-linux-musl/lib/ldscripts/",
    "var/cache/apk/",
)

def rel(path: Path) -> str:
    return path.relative_to(src).as_posix()

def wanted(path: Path) -> bool:
    name = rel(path)
    usr_bin_allow = {
        "ar", "as", "c++filt", "clang", "clang++", "clang++-20", "clang-20",
        "clang-cpp", "ld", "ld.bfd", "nm", "objcopy", "objdump", "ranlib",
        "readelf", "size", "strings", "strip",
    }
    if name.startswith("usr/bin/") and name.removeprefix("usr/bin/") not in usr_bin_allow:
        return False
    if name in exclude:
        return False
    if any(name == item or name.startswith(item + "/") for item in exclude_dirs):
        return False
    if any(name.startswith(prefix) for prefix in exclude_prefixes):
        return False
    if name.startswith("usr/lib/lib") and ".so." in name:
        leaf = name.rsplit("/", 1)[-1]
        keep = {
            "libclang-cpp.so.20.1",
            "libctf-nobfd.so.0.0.0",
            "libctf.so.0.0.0",
            "libffi.so.8.1.4",
            "libjansson.so.4.14.1",
            "libLLVM.so.20.1",
            "liblzma.so.5.8.3",
            "libsframe.so.1.0.0",
            "libstdc++.so.6.0.33",
            "libxml2.so.2.13.9",
            "libz.so.1.3.2",
            "libzstd.so.1.5.7",
        }
        if leaf not in keep and leaf.count(".") >= 4:
            return False
    keep_static = {
        "usr/lib/gcc/x86_64-alpine-linux-musl/14.2.0/libgcc.a",
        "usr/lib/gcc/x86_64-alpine-linux-musl/14.2.0/libgcc_eh.a",
        "usr/lib/libssp_nonshared.a",
    }
    if name.endswith(".a") and name not in keep_static:
        return False
    return True

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
            if rel(item) == "lib/libc.musl-x86_64.so.1":
                shutil.copy2(target, out)
            else:
                out.write_bytes(b"CAPABILITYOS_ROOTFS_SYMLINK\n" + target_text.encode())
        elif item.is_file():
            shutil.copy2(item, out)

PY

rm -rf "$output_root"
mv "$output_root.tmp" "$output_root"
printf '%s\n' "$config_id" > "$stamp"
