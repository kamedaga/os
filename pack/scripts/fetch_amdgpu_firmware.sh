#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch only the pinned local candidate set. No boot media is modified.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
manifest="$repo_root/kobox2/linux-sandbox/kobox/manifest/profiles/amdgpu-firmware.sha256"
destination="${1:-$repo_root/.artifacts/kobox2-amdgpu-firmware}"
tag=20260916

mkdir -p "$destination/amdgpu"
while read -r digest name; do
    [[ -z "${digest:-}" || "$digest" == \#* ]] && continue
    if [[ ! "$digest" =~ ^[0-9a-f]{64}$ ||
          ! "$name" =~ ^amdgpu/[a-z0-9_]+\.bin$ ]]; then
        echo "Invalid pinned firmware entry: $name" >&2
        exit 1
    fi
    target="$destination/$name"
    if [[ -e "$target" ]]; then
        if printf '%s  %s\n' "$digest" "$target" | sha256sum -c - >/dev/null; then
            continue
        fi
        echo "Existing firmware has the wrong digest; refusing to replace: $target" >&2
        exit 1
    fi
    curl --fail --location --retry 2 --silent --show-error \
        "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$tag/$name" \
        --output "$target.download"
    printf '%s  %s\n' "$digest" "$target.download" | sha256sum -c -
    mv "$target.download" "$target"
done < "$manifest"

(cd "$destination" && sha256sum --check "$manifest")
