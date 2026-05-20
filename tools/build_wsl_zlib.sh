#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
prefix="$root_dir/.artifacts/zlib-musl"
version="1.3.2"
archive="$src_dir/zlib-$version.tar.gz"
build_dir="$src_dir/zlib-$version"
output="$root_dir/.artifacts/userland-fixtures/libz.so.1"
stamp="$build_dir/.capabilityos-built"
cc_stamp="$build_dir/.capabilityos-cc"
config_stamp="$build_dir/.capabilityos-config"
config_id="musl-shared-zlib-$version-no-symbol-version-sysv-static-archive-v3"

mkdir -p "$src_dir" "$prefix" "$(dirname "$output")"

if [ ! -f "$archive" ]; then
  wget -O "$archive" "https://zlib.net/zlib-$version.tar.gz"
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

if [ -f "$stamp" ] &&
   [ -f "$prefix/lib/libz.so.1" ] &&
   [ -f "$prefix/lib/libz.a" ] &&
   [ "$(cat "$cc_stamp" 2>/dev/null || true)" = "$cc" ] &&
   [ "$(cat "$config_stamp" 2>/dev/null || true)" = "$config_id" ]; then
  cp "$prefix/lib/libz.so.1" "$output"
  exit 0
fi

make distclean >/dev/null 2>&1 || true
rm -rf "$prefix"
mkdir -p "$prefix"

CC="$cc" CFLAGS="-fPIC -O2" ./configure --shared --prefix="$prefix"
make -j"$(nproc)" V=0

"$cc" -shared -Wl,--hash-style=sysv -Wl,-soname,libz.so.1 -o libz.so.1.3.2 \
  adler32.o \
  crc32.o \
  deflate.o \
  infback.o \
  inffast.o \
  inflate.o \
  inftrees.o \
  trees.o \
  zutil.o \
  compress.o \
  uncompr.o \
  gzclose.o \
  gzlib.o \
  gzread.o \
  gzwrite.o \
  -lc

mkdir -p "$prefix/lib/pkgconfig" "$prefix/include"
cp zlib.h zconf.h "$prefix/include/"
cp libz.so.1.3.2 "$prefix/lib/libz.so.1"
cp libz.so.1.3.2 "$prefix/lib/libz.so"
cp libz.a "$prefix/lib/libz.a"
chmod 755 "$prefix/lib/libz.so.1" "$prefix/lib/libz.so"
cat > "$prefix/lib/pkgconfig/zlib.pc" <<ZLIB_PC
prefix=$prefix
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: zlib
Description: zlib compression library
Version: $version
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
ZLIB_PC

cp "$prefix/lib/libz.so.1" "$output"
printf '%s\n' "$cc" > "$cc_stamp"
printf '%s\n' "$config_id" > "$config_stamp"
touch "$stamp"
