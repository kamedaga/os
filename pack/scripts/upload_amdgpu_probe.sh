#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Upload an optional, already-built GPU package to a running live system.
# The target and SSH identity are explicit: no machine or LAN address is built in.
set -euo pipefail

usage() {
    echo "usage: $0 root@host ssh-identity [package-dir] [--probe]" >&2
    exit 2
}

[[ $# -ge 2 && $# -le 4 ]] || usage
target="$1"
identity="$2"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
package_dir="${3:-$repo_root/.artifacts/kobox2-amdgpu-package}"
probe=0
if [[ "$package_dir" == --probe ]]; then
    package_dir="$repo_root/.artifacts/kobox2-amdgpu-package"
    probe=1
fi
if [[ ${4:-} == --probe ]]; then probe=1; elif [[ $# -eq 4 ]]; then usage; fi
[[ "$target" == root@* && -f "$identity" && -f "$package_dir/owner-policy.conf" ]] || usage

remote_root=/srv/kobox2/amdgpu
chunk_bytes=$((4 * 1024 * 1024))
session_dir="$(mktemp -d)"
trap 'rm -r -- "$session_dir"' EXIT
ssh_options=(-i "$identity" -o BatchMode=yes -o IdentitiesOnly=yes
    -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new
    -o "UserKnownHostsFile=$session_dir/known_hosts" -o LogLevel=ERROR)
# The current live profile generates its Dropbear host key in RAM on each
# boot. A private, per-upload known-hosts file pins the key for every chunk,
# but its first connection is still trust-on-first-use. Use this test path only
# on the intended LAN until the installer has a persistent host identity.
remote() { ssh "${ssh_options[@]}" "$target" "$1"; }
digest() { sha256sum "$1" | cut -d' ' -f1; }
remote_digest() { remote "busybox sha256sum '$1'" | cut -d' ' -f1; }

policy="$package_dir/owner-policy.conf"
[[ $(sed -n '1p' "$policy") == "package_root=$remote_root" ]] || {
    echo 'Package policy has an unexpected RAM root' >&2; exit 1;
}
for item in firmware.bundle manifest.bin sandbox.elf; do
    case "$item" in
        firmware.bundle) key=firmware_sha256 ;;
        manifest.bin) key=manifest_sha256 ;;
        sandbox.elf) key=sandbox_sha256 ;;
    esac
    expected="$(sed -n "s/^${key}=//p" "$policy")"
    [[ "$expected" == "$(digest "$package_dir/$item")" ]] || {
        echo "Policy digest mismatch: $item" >&2; exit 1;
    }
done

remote "busybox mkdir -p '$remote_root/modules'"
# A fresh optional live image already carries this package. Verify the
# embedded archive before expansion, then retain the per-file checks below.
# If the host was rebuilt since USB publication, only changed files travel.
embedded=/srv/amdgpu-driver-package.tar.gz
if remote "busybox test ! -f '$remote_root/manifest.bin'"; then
    embedded_expected="$(remote "busybox cat '$embedded.sha256'" 2>/dev/null || :)"
    if [[ "$embedded_expected" =~ ^[0-9a-f]{64}$ ]] &&
       [[ "$(remote_digest "$embedded" 2>/dev/null || :)" == "$embedded_expected" ]]; then
        remote "busybox tar -xzf '$embedded' -C '$remote_root'"
        echo 'Verified bootfs GPU baseline expanded in RAM; checking for changed files.'
    fi
fi
upload_file() {
    local relative="$1" source="$package_dir/$1" destination="$remote_root/$1"
    local expected size count block actual
    [[ "$relative" =~ ^(modules/)?[A-Za-z0-9_.-]+$ && -f "$source" ]] || {
        echo "Invalid package file: $relative" >&2; exit 1;
    }
    expected="$(digest "$source")"
    if [[ "$(remote_digest "$destination" 2>/dev/null || :)" == "$expected" ]]; then
        echo "verified existing $relative"
        return
    fi
    size="$(stat -c %s "$source")"
    count=$(((size + chunk_bytes - 1) / chunk_bytes))
    remote "busybox sh -c ': > \"$destination\"'"
    for ((block = 0; block < count; ++block)); do
        # Bounded SSH streams avoid a single long-lived channel holding the
        # entire closure. A failed chunk aborts before policy or probe.
        dd if="$source" bs="$chunk_bytes" skip="$block" count=1 status=none |
            timeout 30s ssh "${ssh_options[@]}" "$target" \
                "busybox dd of='$destination' bs=$chunk_bytes seek=$block conv=notrunc" \
                2>/dev/null
    done
    actual="$(remote_digest "$destination")"
    [[ "$actual" == "$expected" ]] || {
        echo "Remote digest mismatch: $relative" >&2; exit 1;
    }
    echo "verified $relative ($size bytes)"
}

upload_file manifest.bin
upload_file sandbox.elf
upload_file core.so
for module in "$package_dir"/modules/*.ko; do
    [[ -f "$module" ]] || { echo 'No GPU modules in package' >&2; exit 1; }
    upload_file "modules/${module##*/}"
done
upload_file firmware.bundle

# The policy is written last. gpud reads it only after an explicit probe
# command; incomplete uploads can never authorize a partial closure.
timeout 30s ssh "${ssh_options[@]}" "$target" \
    'busybox dd of=/run/pacha/gpu-probe.conf bs=4096' \
    < "$policy" 2>/dev/null
[[ "$(remote_digest /run/pacha/gpu-probe.conf)" == "$(digest "$policy")" ]] || {
    echo 'Remote RAM policy digest mismatch' >&2; exit 1;
}
echo 'GPU package and RAM policy verified; no probe has run.'
if ((probe)); then
    remote "busybox printf 'probe\\n' > /run/pacha/gpu-control"
    echo 'One-shot GPU probe requested. Inspect /run/pacha/service.log.'
fi
