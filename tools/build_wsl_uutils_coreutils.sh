#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
version="0.4.0"
target="x86_64-unknown-linux-musl"
archive_name="coreutils-$version-$target.tar.gz"
archive="$src_dir/$archive_name"
extract_dir="$src_dir/coreutils-$version-$target"
output="$root_dir/.artifacts/userland-fixtures/uutils-coreutils.elf"
expected_sha256="7658be348de0741308f59ddc0ceec0c84a879b5fad575226c4628a5ecd39c06d"

mkdir -p "$src_dir" "$(dirname "$output")"

if [ ! -f "$archive" ]; then
  wget -O "$archive" "https://github.com/uutils/coreutils/releases/download/$version/$archive_name"
fi

actual_sha256="$(sha256sum "$archive" | awk '{print $1}')"
if [ "$actual_sha256" != "$expected_sha256" ]; then
  echo "uutils archive sha256 mismatch: $actual_sha256" >&2
  exit 1
fi

if [ ! -x "$extract_dir/coreutils" ]; then
  rm -rf "$extract_dir"
  tar -xzf "$archive" -C "$src_dir"
fi

cp "$extract_dir/coreutils" "$output"
