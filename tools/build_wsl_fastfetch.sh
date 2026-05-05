#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
version="2.61.0"
archive="$src_dir/fastfetch-$version.tar.gz"
build_dir="$src_dir/fastfetch-$version"
cmake_dir="$build_dir/cap-build"
output="$root_dir/userland/fixtures/fastfetch.elf"
stamp="$cmake_dir/.capabilityos-built"
cc_stamp="$cmake_dir/.capabilityos-cc"
config_stamp="$cmake_dir/.capabilityos-config"
config_id="musl-pie-minimal-version-v4-kernel-uapi-asm"

mkdir -p "$src_dir" "$root_dir/userland/fixtures"

if [ ! -f "$archive" ]; then
  wget -O "$archive" "https://github.com/fastfetch-cli/fastfetch/archive/refs/tags/$version.tar.gz"
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

mkdir -p "$cmake_dir"

if [ -f "$stamp" ] && [ -x "$cmake_dir/fastfetch" ] && [ "$(cat "$cc_stamp" 2>/dev/null || true)" = "$cc" ] && [ "$(cat "$config_stamp" 2>/dev/null || true)" = "$config_id" ]; then
  if [ ! -f "$output" ] || [ "$cmake_dir/fastfetch" -nt "$output" ]; then
    cp "$cmake_dir/fastfetch" "$output"
  fi
  exit 0
fi

cmake -S "$build_dir" -B "$cmake_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$cc" \
  -DCMAKE_C_FLAGS="-idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu" \
  -DCMAKE_EXE_LINKER_FLAGS="-pie -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-rpath,/lib -pthread" \
  -DBUILD_FLASHFETCH=OFF \
  -DBINARY_LINK_TYPE=dlopen \
  -DENABLE_ZLIB=OFF \
  -DENABLE_SYSTEM_YYJSON=OFF

cmake --build "$cmake_dir" --target fastfetch -j"$(nproc)"

cp "$cmake_dir/fastfetch" "$output"
printf '%s\n' "$cc" > "$cc_stamp"
printf '%s\n' "$config_id" > "$config_stamp"
touch "$stamp"
