#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -r -- "$scratch"' EXIT
export AMDGPU_TEST_REMOTE="$scratch/guest"
package="$scratch/package"
mkdir -p "$package/modules" "$AMDGPU_TEST_REMOTE/srv" "$AMDGPU_TEST_REMOTE/run/pacha" "$scratch/bin"
cp "$root/tests/amdgpu_upload_mock_ssh.sh" "$scratch/bin/ssh"
chmod +x "$scratch/bin/ssh"
export PATH="$scratch/bin:$PATH"
touch "$scratch/identity"
for file in manifest.bin sandbox.elf firmware.bundle core.so modules/a.ko; do
    printf 'fixture %s\n' "$file" > "$package/$file"
done
{
    echo 'package_root=/srv/kobox2/amdgpu'
    for item in firmware manifest sandbox; do
        case "$item" in firmware) file=firmware.bundle;; manifest) file=manifest.bin;; sandbox) file=sandbox.elf;; esac
        printf '%s_sha256=%s\n' "$item" "$(sha256sum "$package/$file" | cut -d' ' -f1)"
    done
} > "$package/owner-policy.conf"
bash "$root/pack/scripts/build_amdgpu_driver_archive.sh" "$package" "$AMDGPU_TEST_REMOTE/srv/amdgpu-driver-package.tar.gz" >/dev/null
# Host changes after USB publication must transfer only the changed file.
printf 'updated core\n' > "$package/core.so"
bash "$root/pack/scripts/upload_amdgpu_probe.sh" root@test "$scratch/identity" "$package" > "$scratch/log"
[[ "$(grep -c '^verified .*bytes)' "$scratch/log")" == 1 ]]
grep -q '^verified core.so (' "$scratch/log"
grep -q '^verified existing firmware.bundle' "$scratch/log"
for file in manifest.bin sandbox.elf firmware.bundle core.so modules/a.ko; do
    cmp "$package/$file" "$AMDGPU_TEST_REMOTE/srv/kobox2/amdgpu/$file"
done
cmp "$package/owner-policy.conf" "$AMDGPU_TEST_REMOTE/run/pacha/gpu-probe.conf"
[[ ! -e "$AMDGPU_TEST_REMOTE/run/pacha/gpu-control" ]]
bash "$root/pack/scripts/upload_amdgpu_probe.sh" root@test "$scratch/identity" "$package" > "$scratch/repeat"
if grep -q '^verified .*bytes)' "$scratch/repeat"; then exit 1; fi
echo 'AMDGPU upload: bootfs baseline, changed-file-only and repeat reuse PASS'
