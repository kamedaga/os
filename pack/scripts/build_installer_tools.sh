#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
stage="$repo_root/.artifacts/installer-tools"
python3 "$repo_root/pack/scripts/alpine_packages.py" materialize \
    --lock "$repo_root/tools/manifests/alpine-installer-v3.22-x86_64.lock" \
    --cache "$repo_root/.artifacts/alpine-installer-apk" \
    --output "$stage"

for tool in "$stage/sbin/mkfs.fat" "$stage/sbin/mke2fs" \
    "$stage/sbin/e2fsck"; do
    readelf -l "$tool" | rg -q '/lib/ld-musl-x86_64.so.1'
done
readelf -l "$stage/usr/bin/mtools" | rg -q '/lib/ld-musl-x86_64.so.1'
for command in mcopy mmd mdir; do test -e "$stage/usr/bin/$command"; done
for dependency in libblkid.so.1 libeconf.so.0 libcom_err.so.2 \
    libe2p.so.2 libext2fs.so.2 libuuid.so.1; do
    test -e "$stage/usr/lib/$dependency"
done
test -f "$stage/etc/mke2fs.conf"
