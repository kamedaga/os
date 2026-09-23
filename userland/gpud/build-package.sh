#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="${GPUD_CANONICAL_BUILD:-$repo_root/.artifacts/kobox2-client-exec-canonical}"
provider="${GPUD_PROVIDER_BUILD:-$repo_root/.artifacts/kobox2-client-native-module-provider}"
runtime="$repo_root/.artifacts/gpud-production-runtime"
modules="$repo_root/.artifacts/gpud-production-modules"
for required in "$canonical/vmlinux.a"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing canonical Linux input: $required" >&2
    exit 1
  fi
done
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
  --source-tree "$source_tree" --canonical-build-dir "$canonical" \
  --provider-build-dir "$provider" --output-dir "$runtime" --link --jobs "${JOBS:-2}"
python3 "$source_tree/kobox/boot/build_gem_modules.py" \
  --source-tree "$source_tree" --canonical-build-dir "$canonical" \
  --provider-build-dir "$provider" --output-dir "$modules" --core-dir "$runtime" \
  --virtio-gpu --jobs "${JOBS:-2}"
