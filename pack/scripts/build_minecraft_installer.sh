#!/usr/bin/env bash
# Build only files under .artifacts; never write a USB/NVMe or sync the developer rootfs.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
key="${1:?usage: build_minecraft_installer.sh SSH_PUBLIC_KEY}"
output=.artifacts/minecraft-installer
mkdir -p "$output"
./pacgo build userland --no-rootfs \
  seed0boot seed0root storage_boot filed linux_lpr_runtime unixd termd inputd \
  usbd netd logd gpud pachaos_musl_ldso pachaos_musl_libc lpr_linux_musl_libc \
  live_dropbear live_dropbearkey amdgpu_probe_policy amdgpu_mesa_runtime
python3 - <<'PY_OVERLAY'
from pathlib import Path
import hashlib,re
source=Path('pack/live-amdgpu-probe.yaml').read_text()
with open('.artifacts/kobox2-amdgpu-mesa-runtime.tar.gz','rb') as f:
    digest=hashlib.file_digest(f,'sha256').hexdigest()
source,count=re.subn(r'(?m)^  amdgpu_mesa_runtime: [0-9a-f]{64}$','  amdgpu_mesa_runtime: '+digest,source)
assert count==1
Path('.artifacts/minecraft-installer/gpu-overlay.yaml').write_text(source)
PY_OVERLAY
# Resolve all installer services and matching core/adapter packages before
# composing the target archive and SYSTEM.BOOTFS from their current artifacts.
./pacgo build live-bootfs --installer --ssh-authorized-key-file "$key" \
  --overlay-spec "$output/gpu-overlay.yaml" --output-dir "$output/live"
python3 pack/scripts/build_live_desktop_bundle.py --output "$output/DESKTOP.TGZ"
python3 pack/scripts/build_minecraft_installer_rootfs.py \
  --ssh-authorized-key-file "$key" --desktop-bundle "$output/DESKTOP.TGZ"
./pacgo gen manifests
python3 - <<'PY'
from pathlib import Path
import struct,uuid
base=Path('.artifacts/minecraft-installer')
identity=base/'root-uuid.bin'
if not identity.exists():identity.write_bytes(uuid.uuid4().bytes)
assert len(identity.read_bytes())==16 and any(identity.read_bytes())
(base/'root-uuid.txt').write_text(str(uuid.UUID(bytes=identity.read_bytes()))+'\n')
(base/'storage-policy.bin').write_bytes(struct.pack('<QQQ',512<<20,64,16<<20))
manifest=Path('.artifacts/manifests/bootfs.generated.txt')
lines=[]
for line in manifest.read_text().splitlines():
 if not line or line.startswith('#'):continue
 target,source=line.split('=',1)
 if target in ('/etc/pacha/root-uuid','/etc/pacha/storage-policy'):continue
 lines.append(target+'='+str((manifest.parent/source).resolve()))
lines+=['/etc/pacha/root-uuid='+str(identity.resolve()),
        '/etc/pacha/storage-policy='+str((base/'storage-policy.bin').resolve())]
(base/'system-bootfs.manifest').write_text('\n'.join(lines)+'\n')
PY
(cd pack && go run ./cmd/bootfs-image \
  -manifest ../"$output/system-bootfs.manifest" -output ../"$output/SYSTEM.BOOTFS")
./pacgo build live-image --ssh-authorized-key-file "$key" \
  --overlay-spec "$output/gpu-overlay.yaml" \
  --installer-rootfs "$output/ROOTFS.TAR" \
  --installer-system-bootfs "$output/SYSTEM.BOOTFS" \
  --live-desktop-bundle "$output/DESKTOP.TGZ" --output-dir "$output/media"
sha256sum "$output/media/limine-boot.img" > "$output/media/limine-boot.img.sha256"
