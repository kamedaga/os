#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
pkg_name="apk-tools-static-2.14.9-r3.apk"
pkg_url="https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/$pkg_name"
pkg_path="$src_dir/$pkg_name"
extract_dir="$src_dir/apk-tools-static-2.14.9-r3"
output="$root_dir/.artifacts/userland-fixtures/apk.elf"

mkdir -p "$src_dir" "$extract_dir" "$(dirname "$output")"

if [ ! -f "$pkg_path" ]; then
  if command -v wget >/dev/null 2>&1; then
    wget -O "$pkg_path" "$pkg_url"
  elif command -v curl >/dev/null 2>&1; then
    curl -L -o "$pkg_path" "$pkg_url"
  else
    echo "missing downloader: install wget or curl in WSL" >&2
    exit 1
  fi
fi

if [ ! -f "$extract_dir/sbin/apk.static" ]; then
  tar -xzf "$pkg_path" -C "$extract_dir"
fi

cp "$extract_dir/sbin/apk.static" "$output"
chmod 755 "$output"
