#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Stage a verified PCI DRM package; this never writes an installed rootfs.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$repo_root" --command bash "$0" "$@"
fi
export PATH="$CAPOS_LINUX_LLVM18_BIN:$PATH"
export HOSTCFLAGS="${HOSTCFLAGS:-} -isystem$CAPOS_LIBELF_INCLUDE"
export LIBRARY_PATH="$CAPOS_LIBELF_LIB${LIBRARY_PATH:+:$LIBRARY_PATH}"
source_tree="$repo_root/kobox2/linux-sandbox"
output="$repo_root/.artifacts/intel-gpu-bringup"
package="$output/package"
if [[ $# -ne 0 ]]; then echo "usage: $0 (optional I915_VBT_FILE environment)" >&2; exit 2; fi
bash "$repo_root/pack/scripts/build_i915_profile.sh"
bash "$repo_root/pack/scripts/fetch_i915_firmware.sh"
canonical="$output/i915-canonical"
identity="$(sha256sum "$canonical/.config" "$canonical/vmlinux" \
    "$canonical/vmlinux.a" | sha256sum | cut -d' ' -f1)"
mapping="$output/mapping"
python3 "$source_tree/kobox/boot/build_gem_modules.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$output/providers/$identity" \
    --output-dir "$mapping" --core-dir "$output/runtime-production" \
    --mapping-only --jobs "${JOBS:-4}"
python3 "$source_tree/kobox/manifest/build_pci_package.py" \
    --profile i915 --inventory "$output/closure-inventory.json" \
    --runtime-dir "$output/runtime-production" --output-dir "$package" \
    --generator-dir "$output/package-generator" --protocol-dir "$repo_root/kobox2/protocol" \
    --extension-module "$mapping/kobox/gem/drm_mapping.ko"
python3 - "$repo_root" "${I915_VBT_FILE:-}" <<'PY'
from pathlib import Path
import hashlib,sys
root=Path(sys.argv[1]);out=root/'.artifacts/intel-gpu-bringup'
pinned=root/'kobox2/linux-sandbox/kobox/manifest/profiles/i915-firmware.sha256'
manifest=pinned.read_text()
if sys.argv[2]:
    content=Path(sys.argv[2]).read_bytes()
    if not content.startswith(b'$VBT') or not 48 <= len(content) <= 8 << 20:
        raise ValueError('invalid board VBT extent/signature')
    target=out/'firmware/i915/nec-xc750dag.vbt'
    target.write_bytes(content)
    manifest += hashlib.sha256(content).hexdigest()+'  i915/nec-xc750dag.vbt\n'
(out/'package/firmware.sha256').write_text(manifest)
PY
python3 "$repo_root/pack/scripts/build_amdgpu_firmware_bundle.py" \
    --profile i915 --manifest "$package/firmware.sha256" \
    --firmware-dir "$output/firmware" --output "$package/firmware.bundle"
bash "$repo_root/userland/kobox2_adapter/build-sandbox.sh" "$package/sandbox.elf" --i915
python3 - "$package" <<'PY'
from pathlib import Path
import hashlib,sys
package=Path(sys.argv[1])
lines=['package_root=/srv/kobox2/i915']
for key,file in [('firmware','firmware.bundle'),('manifest','manifest.bin'),('sandbox','sandbox.elf')]:
    lines.append(key+'_sha256='+hashlib.sha256((package/file).read_bytes()).hexdigest())
(package/'gpu-probe.conf').write_text('\n'.join(lines)+'\n')
PY
printf 'i915 package staged: %s\n' "$package"
