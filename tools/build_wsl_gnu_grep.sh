#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
version="3.12"
archive="$src_dir/grep-$version.tar.xz"
build_dir="$src_dir/grep-$version"
output="$root_dir/.artifacts/userland-fixtures/gnu-grep.elf"
expected_sha256="2649b27c0e90e632eadcd757be06c6e9a4f48d941de51e7c0f83ff76408a07b9"
stamp="$build_dir/.capabilityos-built"
cc_stamp="$build_dir/.capabilityos-cc"
config_stamp="$build_dir/.capabilityos-config"
config_id="musl-pie-gnu-grep-3.12-v1"

mkdir -p "$src_dir" "$(dirname "$output")"

if [ ! -f "$archive" ]; then
  if command -v wget >/dev/null 2>&1; then
    wget -O "$archive" "https://ftp.gnu.org/gnu/grep/grep-$version.tar.xz"
  elif command -v curl >/dev/null 2>&1; then
    curl -L -o "$archive" "https://ftp.gnu.org/gnu/grep/grep-$version.tar.xz"
  else
    echo "missing downloader: install wget or curl in WSL" >&2
    exit 1
  fi
fi

actual_sha256="$(sha256sum "$archive" | awk '{print $1}')"
if [ "$actual_sha256" != "$expected_sha256" ]; then
  echo "GNU grep source archive sha256 mismatch: $actual_sha256" >&2
  exit 1
fi

if command -v musl-gcc >/dev/null 2>&1; then
  cc=musl-gcc
elif command -v musl-clang >/dev/null 2>&1; then
  cc=musl-clang
else
  echo "missing musl toolchain: install musl-tools in WSL" >&2
  exit 1
fi

if [ ! -d "$build_dir" ]; then
  tar -xJf "$archive" -C "$src_dir"
fi

cd "$build_dir"

if [ -f "$stamp" ] && [ -x src/grep ] && [ "$(cat "$cc_stamp" 2>/dev/null || true)" = "$cc" ] && [ "$(cat "$config_stamp" 2>/dev/null || true)" = "$config_id" ]; then
  if [ ! -f "$output" ] || [ src/grep -nt "$output" ]; then
    cp src/grep "$output"
  fi
  exit 0
fi

make distclean >/dev/null 2>&1 || true

CC="$cc" \
CFLAGS="-fPIE -O2" \
LDFLAGS="-pie -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-rpath,/lib" \
./configure \
  --host=x86_64-linux-musl \
  --prefix=/usr \
  --disable-nls \
  --disable-perl-regexp

make -j"$(nproc)" V=0

cp src/grep "$output"
chmod 755 "$output"
printf '%s\n' "$cc" > "$cc_stamp"
printf '%s\n' "$config_id" > "$config_stamp"
touch "$stamp"
