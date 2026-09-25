#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_dir="$repo_root/.artifacts/third_party/libvterm-0.3.3/source"
build_dir="$repo_root/.artifacts/third_party/libvterm-0.3.3/build"
revision="9d6d2112335080312ef8c36667fa717ded4f7daf"
font_source="/usr/share/consolefonts/Lat15-VGA16.psf.gz"
font_hash="3818f6f8a805515ed24283b8fa050278af877813782b980509d22cad969634c8"
font_package="$repo_root/.artifacts/third_party/console-setup-linux_1.226ubuntu1_all.deb"
font_package_hash="d00a8dfd142a1af64c0346b2c8d47c9bd412e15c6b63cf3a42606029bdb862e7"

if [[ ! -d "$source_dir/.git" ]]; then
  mkdir -p "$(dirname "$source_dir")"
  git clone --filter=blob:none --no-checkout \
    https://github.com/neovim/libvterm.git "$source_dir"
fi
if [[ "$(git -C "$source_dir" rev-parse HEAD 2>/dev/null || true)" != "$revision" ]]; then
  git -C "$source_dir" fetch --depth=1 origin "$revision"
  git -C "$source_dir" checkout --detach "$revision"
fi
[[ "$(git -C "$source_dir" rev-parse HEAD)" == "$revision" ]]
if [[ ! -f "$font_source" ]] ||
    ! printf '%s  %s\n' "$font_hash" "$font_source" | sha256sum -c - >/dev/null 2>&1; then
  font_root="$repo_root/.artifacts/third_party/console-setup-linux-1.226ubuntu1"
  if [[ ! -f "$font_package" ]] ||
      ! printf '%s  %s\n' "$font_package_hash" "$font_package" | sha256sum -c - >/dev/null 2>&1; then
    curl -fL --retry 3 -o "$font_package" \
      https://archive.ubuntu.com/ubuntu/pool/main/c/console-setup/console-setup-linux_1.226ubuntu1_all.deb
  fi
  printf '%s  %s\n' "$font_package_hash" "$font_package" | sha256sum -c -
  mkdir -p "$font_root"
  dpkg-deb -x "$font_package" "$font_root"
  font_source="$font_root/usr/share/consolefonts/Lat15-VGA16.psf.gz"
fi
printf '%s  %s\n' "$font_hash" "$font_source" | sha256sum -c -

# Generate upstream's build-time tables in a separate build copy; keep its
# checkout unchanged, as required for Linux-compatible third-party inputs.
mkdir -p "$build_dir/src" "$build_dir/include"
cp -a "$source_dir/src/." "$build_dir/src/"
cp -a "$source_dir/include/." "$build_dir/include/"
perl -CSD "$source_dir/tbl2inc_c.pl" \
  "$source_dir/src/encoding/DECdrawing.tbl" > "$build_dir/src/encoding/DECdrawing.inc"
perl -CSD "$source_dir/tbl2inc_c.pl" \
  "$source_dir/src/encoding/uk.tbl" > "$build_dir/src/encoding/uk.inc"
gzip -cd "$font_source" > "$build_dir/font.psf"
(cd "$build_dir" && xxd -i font.psf > font_psf.h)
printf '%s\n' "$build_dir"
