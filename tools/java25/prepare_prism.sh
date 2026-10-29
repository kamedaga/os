#!/usr/bin/env bash
# Extract the official AppImage on the host; the guest needs no FUSE service.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$repo_root/.artifacts/java25/minecraft-26.1.2"
cache="$repo_root/.artifacts/third_party/prism"
app="$cache/PrismLauncher-Linux-x86_64.AppImage"
digest=bb81038c56a09e944659e4b808dbfbc52d52d5e3a3ffe32216689a3ca1508d3d
mkdir -p "$cache" "$out/prism-extract" "$out/prism-root/usr/local/bin" \
  "$out/prism-root/usr/share/applications" "$out/prism-root/opt/prismlauncher/extra" \
  "$out/prism-root/opt/java25-openj9/glibc/locale"
if ! printf '%s  %s\n' "$digest" "$app" | sha256sum --check --status; then
  curl --fail --location --retry 2 \
    https://github.com/PrismLauncher/PrismLauncher/releases/download/11.1.1/PrismLauncher-Linux-x86_64.AppImage \
    -o "$app.download"
  printf '%s  %s\n' "$digest" "$app.download" | sha256sum --check
  mv "$app.download" "$app"
fi
chmod 755 "$app"
cd "$out/prism-extract"
"$app" --appimage-extract > "$out/prism-extract.log"
cp "$repo_root/tools/java25/prismlauncher" "$out/prism-root/usr/local/bin/prismlauncher"
chmod 755 "$out/prism-root/usr/local/bin/prismlauncher"
# Qt supports the OpenSSL 3 ABI. Use the unmodified 3.5 pair already supplied
# by the installed Semeru JRE, retaining its matching DT_NEEDED name too.
ln -sfn /opt/java25-openj9/lib/libssl-semeru.so "$out/prism-root/opt/prismlauncher/extra/libssl.so.3"
ln -sfn /opt/java25-openj9/lib/libcrypto-semeru.so "$out/prism-root/opt/prismlauncher/extra/libcrypto.so.3"
ln -sfn /opt/java25-openj9/lib/libcrypto-semeru.so "$out/prism-root/opt/prismlauncher/extra/libcrypto-semeru.so"
cp -a /usr/lib/locale/C.utf8 "$out/prism-root/opt/java25-openj9/glibc/locale/"
# The AppImage's real binary is under shared/bin; retain its standard
# ../share/PrismLauncher lookup without running the AppImage mount wrapper.
mkdir -p "$out/prism-root/opt/prismlauncher/shared"
ln -sfn ../share "$out/prism-root/opt/prismlauncher/shared/share"
cat > "$out/prism-root/usr/share/applications/prismlauncher.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Prism Launcher
Exec=prismlauncher
Icon=/opt/prismlauncher/org.prismlauncher.PrismLauncher.png
Categories=Game;
Terminal=false
EOF
python3 - "$out" <<'PY'
from pathlib import Path
import sys,tarfile
out=Path(sys.argv[1])
with tarfile.open(out/'prism.tar.gz','w:gz',compresslevel=1) as archive:
    archive.add(out/'prism-extract/squashfs-root',arcname='opt/prismlauncher')
    for path in sorted((out/'prism-root').rglob('*')):
        if path.is_file() or path.is_symlink():archive.add(path,arcname=str(path.relative_to(out/'prism-root')))
print('Prepared unmodified Prism Launcher 11.1.1 for the guest.')
PY
