#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
prefix="$root_dir/.artifacts/mbedtls-musl"
version="3.6.6"
archive="$src_dir/mbedtls-$version.tar.bz2"
build_dir="$src_dir/mbedtls-$version"
cmake_build_dir="$build_dir/cap-build"
stamp="$build_dir/.capabilityos-built"
cc_stamp="$build_dir/.capabilityos-cc"
config_stamp="$build_dir/.capabilityos-config"
config_id="musl-shared-mbedtls-$version-sysv-v3"

mkdir -p "$src_dir" "$prefix" "$root_dir/userland/fixtures"

if [ ! -f "$archive" ]; then
  wget -O "$archive" "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$version/mbedtls-$version.tar.bz2"
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
  tar -xjf "$archive" -C "$src_dir"
fi

copy_outputs() {
  cp "$(readlink -f "$prefix/lib/libmbedcrypto.so.16")" "$root_dir/userland/fixtures/libmbedcrypto.so.16"
  cp "$(readlink -f "$prefix/lib/libmbedx509.so.7")" "$root_dir/userland/fixtures/libmbedx509.so.7"
  cp "$(readlink -f "$prefix/lib/libmbedtls.so.21")" "$root_dir/userland/fixtures/libmbedtls.so.21"
  chmod 755 \
    "$root_dir/userland/fixtures/libmbedcrypto.so.16" \
    "$root_dir/userland/fixtures/libmbedx509.so.7" \
    "$root_dir/userland/fixtures/libmbedtls.so.21"
}

if [ -f "$stamp" ] &&
   [ -f "$prefix/lib/libmbedcrypto.so.16" ] &&
   [ -f "$prefix/lib/libmbedx509.so.7" ] &&
   [ -f "$prefix/lib/libmbedtls.so.21" ] &&
   [ -f "$prefix/lib/libmbedtls.a" ] &&
   [ -f "$prefix/lib/libmbedx509.a" ] &&
   [ -f "$prefix/lib/libmbedcrypto.a" ] &&
   [ "$(cat "$cc_stamp" 2>/dev/null || true)" = "$cc" ] &&
   [ "$(cat "$config_stamp" 2>/dev/null || true)" = "$config_id" ]; then
  copy_outputs
  exit 0
fi

rm -rf "$prefix" "$cmake_build_dir"
mkdir -p "$prefix" "$cmake_build_dir"

cmake -S "$build_dir" -B "$cmake_build_dir" \
  -DCMAKE_C_COMPILER="$cc" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_C_FLAGS="-fPIC -O2" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--hash-style=sysv" \
  -DUSE_SHARED_MBEDTLS_LIBRARY=ON \
  -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
  -DENABLE_PROGRAMS=OFF \
  -DENABLE_TESTING=OFF \
  -DMBEDTLS_FATAL_WARNINGS=OFF

cmake --build "$cmake_build_dir" -j"$(nproc)"
cmake --install "$cmake_build_dir"

copy_outputs
printf '%s\n' "$cc" > "$cc_stamp"
printf '%s\n' "$config_id" > "$config_stamp"
touch "$stamp"
