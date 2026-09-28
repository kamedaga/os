#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
package="${1:-$repo_root/.artifacts/kobox2-amdgpu-package}"
output="${2:-$repo_root/.artifacts/kobox2-amdgpu-driver.tar.gz}"
files=(manifest.bin sandbox.elf core.so firmware.bundle owner-policy.conf)
for module in "$package"/modules/*.ko; do
    name="${module##*/}"
    [[ "$name" =~ ^[A-Za-z0-9_.-]+\.ko$ ]] || exit 1
    files+=("modules/$name")
done
for file in "${files[@]}"; do
    [[ -f "$package/$file" && -s "$package/$file" && ! -L "$package/$file" ]] || {
        echo "Missing or non-regular package input: $file" >&2; exit 1;
    }
done
[[ ! -L "$package/modules" ]] || exit 1
mkdir -p "$(dirname "$output")"
# A whitelist avoids including build objects, loose firmware duplicates, or
# an old archive. Deterministic bytes let the host skip all binary transfer.
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C "$package" -I 'gzip -n' -cf "$output" -- "${files[@]}"
sha256sum "$output"
# The optional bootfs publishes this digest beside the archive. It permits
# reuse of that verified baseline even after the host package changes.
sha256sum "$output" | cut -d' ' -f1 > "$output.sha256"
