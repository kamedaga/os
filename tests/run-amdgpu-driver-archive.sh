#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -r -- "$scratch"' EXIT
package="$scratch/package"
mkdir -p "$package/modules" "$package/firmware"
for name in manifest.bin sandbox.elf core.so firmware.bundle owner-policy.conf modules/example.ko; do
    printf 'fixture %s\n' "$name" > "$package/$name"
done
printf 'excluded\n' > "$package/firmware/unused.bin"
bash "$root/pack/scripts/build_amdgpu_driver_archive.sh" "$package" "$scratch/first.tgz" >/dev/null
touch "$package/core.so"
bash "$root/pack/scripts/build_amdgpu_driver_archive.sh" "$package" "$scratch/second.tgz" >/dev/null
cmp "$scratch/first.tgz" "$scratch/second.tgz"
[[ "$(cat "$scratch/first.tgz.sha256")" == "$(sha256sum "$scratch/first.tgz" | cut -d' ' -f1)" ]]
[[ "$(tar -tzf "$scratch/first.tgz" | wc -l)" == 6 ]]
if tar -tzf "$scratch/first.tgz" | grep -q 'unused'; then exit 1; fi
mv "$package/core.so" "$package/core.saved"
ln -s core.saved "$package/core.so"
if bash "$root/pack/scripts/build_amdgpu_driver_archive.sh" "$package" "$scratch/bad.tgz" 2>/dev/null; then
    echo 'symlink accepted' >&2; exit 1
fi
rm "$package/core.so"
mkdir "$package/core.so"
if bash "$root/pack/scripts/build_amdgpu_driver_archive.sh" "$package" "$scratch/bad.tgz" 2>/dev/null; then
    echo 'directory accepted' >&2; exit 1
fi
echo 'AMDGPU driver archive: deterministic, digest, whitelist and non-regular rejection PASS'
