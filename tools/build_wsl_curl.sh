#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
version="8.19.0"
archive="$src_dir/curl-$version.tar.gz"
build_dir="$src_dir/curl-$version"
output="$root_dir/.artifacts/userland-fixtures/curl.elf"
zlib_prefix="$root_dir/.artifacts/zlib-musl"
mbedtls_prefix="$root_dir/.artifacts/mbedtls-musl"
stamp="$build_dir/.capabilityos-built"
cc_stamp="$build_dir/.capabilityos-cc"
config_stamp="$build_dir/.capabilityos-config"
config_id="musl-pie-http-zlib-mbedtls-ca-bundle-v11"

mkdir -p "$src_dir" "$(dirname "$output")"

if [ ! -f "$archive" ]; then
  wget -O "$archive" "https://curl.se/download/curl-$version.tar.gz"
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

bash "$root_dir/tools/build_wsl_zlib.sh"
bash "$root_dir/tools/build_wsl_mbedtls.sh"

cd "$build_dir"

if [ -f "$stamp" ] && [ -x src/curl ] && [ "$(cat "$cc_stamp" 2>/dev/null || true)" = "$cc" ] && [ "$(cat "$config_stamp" 2>/dev/null || true)" = "$config_id" ]; then
  if [ ! -f "$output" ] || [ src/curl -nt "$output" ]; then
    cp src/curl "$output"
  fi
  exit 0
fi

make distclean >/dev/null 2>&1 || true

PKG_CONFIG_PATH="$mbedtls_prefix/lib/pkgconfig:$zlib_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
CC="$cc" \
CPPFLAGS="-I$mbedtls_prefix/include -I$zlib_prefix/include" \
CFLAGS="-fPIE -O2" \
LDFLAGS="-pie -L$mbedtls_prefix/lib -L$zlib_prefix/lib -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-rpath,$mbedtls_prefix/lib -Wl,-rpath,$zlib_prefix/lib -Wl,-rpath,/lib" \
./configure \
  --host=x86_64-linux-musl \
  --prefix=/usr \
  --disable-shared \
  --enable-static \
  --enable-http \
  --disable-ftp \
  --disable-ipfs \
  --disable-ldap \
  --disable-ldaps \
  --disable-rtsp \
  --disable-dict \
  --disable-telnet \
  --disable-tftp \
  --disable-pop3 \
  --disable-imap \
  --disable-smb \
  --disable-smtp \
  --disable-gopher \
  --disable-mqtt \
  --disable-websockets \
  --disable-alt-svc \
  --disable-hsts \
  --disable-manual \
  --disable-ipv6 \
  --disable-threaded-resolver \
  --disable-unix-sockets \
  --with-mbedtls="$mbedtls_prefix" \
  --with-zlib="$zlib_prefix" \
  --without-brotli \
  --without-zstd \
  --without-libpsl \
  --without-libidn2 \
  --without-librtmp \
  --without-nghttp2 \
  --without-ngtcp2 \
  --without-nghttp3 \
  --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
  --without-ca-path

make -j"$(nproc)" V=0

old_runpath="$mbedtls_prefix/lib:$zlib_prefix/lib:/lib"
python3 - "$old_runpath" src/curl <<'PY'
import pathlib
import sys

old = sys.argv[1].encode() + b"\0"
path = pathlib.Path(sys.argv[2])
data = bytearray(path.read_bytes())
pos = data.find(old)
if pos < 0:
    raise SystemExit("curl RUNPATH string not found")
new = b"/lib\0"
data[pos:pos + len(new)] = new
for i in range(pos + len(new), pos + len(old)):
    data[i] = 0
path.write_bytes(data)
PY

cp src/curl "$output"
printf '%s\n' "$cc" > "$cc_stamp"
printf '%s\n' "$config_id" > "$config_stamp"
touch "$stamp"
