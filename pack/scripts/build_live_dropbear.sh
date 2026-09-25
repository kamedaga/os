#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
version=2026.94
expected_sha=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
source_root="$repo_root/.artifacts/third_party/dropbear"
archive="$source_root/dropbear-$version.tar.bz2"
source="$source_root/dropbear-$version"
build="$source_root/build-$version-lpr-ptmx"
output="$repo_root/.artifacts/userland-fixtures/live-dropbear"

mkdir -p "$source_root" "$output"
if [[ ! -f "$archive" ]]; then
  curl -fsSL --retry 3 \
    "https://matt.ucc.asn.au/dropbear/releases/dropbear-$version.tar.bz2" \
    -o "$archive"
fi
printf '%s  %s\n' "$expected_sha" "$archive" | sha256sum -c -
if [[ ! -f "$source/configure" ]]; then
  tar -xjf "$archive" -C "$source_root"
fi
mkdir -p "$build"
if [[ ! -f "$build/Makefile" ]]; then
  (
    cd "$build"
    # musl openpty is followed by ttyname(/proc/self/fd); LPR PTYs expose
    # Linux ptmx ioctls but not a procfd readlink for their native handles.
    # Upstream configure disables its ptmx detection for Linux hosts, so the
    # documented alternative needs an explicit target build definition.
    CC=musl-gcc CPPFLAGS=-DUSE_DEV_PTMX=1 "../dropbear-$version/configure" \
      --enable-static --disable-zlib --disable-syslog \
      --disable-openpty \
      --disable-shadow --disable-utmp --disable-utmpx \
      --disable-wtmp --disable-wtmpx --disable-lastlog
  )
fi
make -s -C "$build" -j 4 PROGRAMS='dropbear dropbearkey'
install -m 0755 "$build/dropbear" "$output/dropbear"
install -m 0755 "$build/dropbearkey" "$output/dropbearkey"
for binary in "$output/dropbear" "$output/dropbearkey"; do
  if readelf -l "$binary" | grep -q INTERP ||
     readelf -d "$binary" 2>/dev/null | grep -q NEEDED; then
    printf 'live Dropbear must be static: %s\n' "$binary" >&2
    exit 1
  fi
done
