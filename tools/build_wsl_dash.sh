#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
build_dir="$src_dir/dash-0.5.12"
tarball="$src_dir/dash-0.5.12.tar.gz"
output="$root_dir/userland/fixtures/dash.elf"
stamp="$build_dir/.capabilityos-built"
cc_stamp="$build_dir/.capabilityos-cc"

mkdir -p "$src_dir" "$root_dir/userland/fixtures"

if [ ! -f "$tarball" ]; then
  wget -O "$tarball" http://gondor.apana.org.au/~herbert/dash/files/dash-0.5.12.tar.gz
fi

if command -v musl-gcc >/dev/null 2>&1; then
  cc=musl-gcc
elif command -v musl-clang >/dev/null 2>&1; then
  cc=musl-clang
else
  echo "missing musl toolchain: install musl-tools in WSL" >&2
  exit 1
fi

if [ -f "$stamp" ] && [ -x "$build_dir/src/dash" ] && [ "$(cat "$cc_stamp" 2>/dev/null || true)" = "$cc" ]; then
  if [ ! -f "$output" ] || [ "$build_dir/src/dash" -nt "$output" ]; then
    cp "$build_dir/src/dash" "$output"
  fi
  exit 0
fi

if [ ! -d "$build_dir" ]; then
  tar -xzf "$tarball" -C "$src_dir"
fi

cd "$build_dir"
if [ ! -f configure ]; then
  ./autogen.sh
fi

if [ ! -f config.status ] || [ "$(cat "$cc_stamp" 2>/dev/null || true)" != "$cc" ]; then
  CC="$cc" ./configure \
    --host=x86_64-linux-musl \
    --prefix=/ \
    --disable-static
  printf '%s\n' "$cc" > "$cc_stamp"
fi

make -j"$(nproc)"
cp src/dash "$output"
touch "$stamp"
