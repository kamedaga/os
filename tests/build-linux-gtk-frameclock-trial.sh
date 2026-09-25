#!/usr/bin/env bash
# User-authorized Linux-only measurement build. Never installs into rootfs.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
work="$repo/.artifacts/gtk3-frameclock-trial"
mkdir -p "$work/downloads" "$work/source" "$work/build" "$work/export"
source_archive="$work/downloads/gtk-3.24.50.tar.xz"
source_sha=40db8b781372d45faebb571363a4919a3259e1bffd110eb91f11d33fcd175eaaf4a4241416dae3398bf0ce58a3d2a9e3dbee1a9153e4e673b45892044be6b9e5
if [[ ! -f "$source_archive" ]]; then
  curl -fL https://download.gnome.org/sources/gtk/3.24/gtk-3.24.50.tar.xz -o "$source_archive"
fi
echo "$source_sha  $source_archive" | sha512sum -c -
bootstrap="$repo/.artifacts/third_party/mesa-25.1.9/alpine-minirootfs.tar.gz"
echo "4b4daa9fe2fc696c4919c4412a4c3d3e770d8fb70292a004a2c72f5096175282  $bootstrap" | sha256sum -c -
toolchain="$work/toolchain"
if [[ ! -f "$toolchain/.ready" ]]; then
  mkdir -p "$toolchain"
  if [[ ! -f "$toolchain/bin/busybox" ]]; then
    tar -xzf "$bootstrap" -C "$toolchain"
  fi
  bwrap --unshare-user --uid 0 --gid 0 --bind "$toolchain" / \
    --dev /dev --proc /proc --tmpfs /tmp \
    --ro-bind /etc/resolv.conf /etc/resolv.conf \
    --ro-bind /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem \
    /sbin/apk --no-cache --no-progress --no-chown add build-base meson ninja \
      gtk+3.0-dev wayland-dev wayland-protocols libxcomposite-dev \
      libxcursor-dev libxdamage-dev libxrandr-dev libxinerama-dev \
      libxkbcommon-dev iso-codes-dev gettext-dev
  touch "$toolchain/.ready"
fi
if [[ ! -f "$work/source-prepared" ]]; then
  tar -xJf "$source_archive" --strip-components=1 -C "$work/source"
  # Retain the two existing Alpine package patches in both trial modes.
  for spec in \
    '5628.patch:b592559177c60e627940b8894aace41b058fe738d47ebb8bccdb4c9c35dd1750e1db7e153a6e5db61d2d4e95ddefd712fd734a1bbc4230a79c889f8adac96e1f' \
    'events-Compress-touch-update-events.patch:74e38bbdf8d8f84bd07c10b1d879d2ef5cc80ffcfc6bcbc3d916c4c4e9a10686e20c77bed4abb56d1eed463c644873ddd2500ef3028beb7db3b10cba3a658909'; do
    name=${spec%%:*}
    curl -fsSL "https://raw.githubusercontent.com/alpinelinux/aports/3.22-stable/main/gtk+3.0/$name" -o "$work/downloads/$name"
    echo "${spec#*:}  $work/downloads/$name" | sha512sum -c -
    patch --batch --fuzz=0 -p1 -d "$work/source" -i "$work/downloads/$name"
  done
  patch --batch --fuzz=0 -p1 -d "$work/source" \
    -i "$repo/tests/patches/gtk-3.24.50-frame-clock-unlimited.patch"
  touch "$work/source-prepared"
fi
mkdir -p "$toolchain/source" "$toolchain/build"
bwrap --unshare-user --uid 0 --gid 0 --ro-bind "$toolchain" / \
  --ro-bind "$work/source" /source --bind "$work/build" /build \
  --dev /dev --proc /proc --tmpfs /tmp --chdir /build \
  --clearenv --setenv PATH /usr/bin:/bin --setenv LC_ALL C /bin/sh -eu -c '
    if [ ! -f build.ninja ]; then
      meson setup /build /source --prefix=/usr --libdir=lib --buildtype=release \
        --wrap-mode=nofallback -Db_lto=true -Dbroadway_backend=true \
        -Dxinerama=yes -Dprint_backends=file,lpr -Dintrospection=false \
        -Dman=false -Dgtk_doc=false -Ddemos=false -Dexamples=false -Dtests=false
    fi
    ninja -j8 gdk/libgdk-3.so.0.2418.32
  '
install -m755 "$work/build/gdk/libgdk-3.so.0.2418.32" "$work/export/libgdk-3.so.0"
sha256sum "$work/export/libgdk-3.so.0"
