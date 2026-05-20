#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
version="1.5.7"
archive="$src_dir/zstd-$version.tar.gz"
build_dir="$src_dir/zstd-$version"
output="$root_dir/.artifacts/userland-fixtures/zstd.elf"
stamp="$build_dir/.capabilityos-built"
cc_stamp="$build_dir/.capabilityos-cc"
config_stamp="$build_dir/.capabilityos-config"
config_id="musl-pie-cli-no-extra-libs-v1"

mkdir -p "$src_dir" "$(dirname "$output")"

if [ ! -f "$archive" ]; then
  wget -O "$archive" "https://github.com/facebook/zstd/releases/download/v$version/zstd-$version.tar.gz"
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
  tar -xzf "$archive" -C "$src_dir"
fi

cd "$build_dir"

if [ -f "$stamp" ] && [ -x zstd ] && [ "$(cat "$cc_stamp" 2>/dev/null || true)" = "$cc" ] && [ "$(cat "$config_stamp" 2>/dev/null || true)" = "$config_id" ]; then
  if [ ! -f "$output" ] || [ zstd -nt "$output" ]; then
    cp zstd "$output"
  fi
  exit 0
fi

make clean
make -j"$(nproc)" zstd \
  CC="$cc" \
  HAVE_ZLIB=0 \
  HAVE_LZMA=0 \
  HAVE_LZ4=0 \
  ZSTD_LEGACY_SUPPORT=0 \
  MOREFLAGS="-fPIE" \
  LDFLAGS="-pie -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-rpath,/lib -pthread"

cp zstd "$output"
printf '%s\n' "$cc" > "$cc_stamp"
printf '%s\n' "$config_id" > "$config_stamp"
touch "$stamp"
