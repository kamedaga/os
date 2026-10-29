#!/usr/bin/env bash
# Run the guest's exact musl/JDK binaries in a rootless Linux mount namespace.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
out="$repo_root/.artifacts/java25"
root="$out/host-root"
jdk="$repo_root/.artifacts/userland-fixtures/java25-root/opt/java25"
musl="$repo_root/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
[[ -x "$jdk/bin/java" && -x "$musl" ]] || {
    echo 'Prepare java25 and copy_lpr_linux_musl first' >&2; exit 1;
}
mkdir -p "$root"/{lib,usr/lib,opt/java25,proc,dev,tmp,tests}
cp "$musl" "$root/lib/ld-musl-x86_64.so.1"
ln -sf ld-musl-x86_64.so.1 "$root/lib/libc.musl-x86_64.so.1"
# Resolve the rootfs marker symlink using the existing overlay helper, rather
# than accidentally loading the host's glibc libz into the musl JVM.
python3 - "$repo_root" "$root" <<'PY'
import shutil, sys
from pathlib import Path
repo, root = map(Path, sys.argv[1:])
sys.path.insert(0, str(repo / 'tools'))
from rootfs_overlay import resolve_payload
roots = [repo / '.artifacts/userland-fixtures/alpine-xfce-root',
         repo / '.artifacts/userland-fixtures/alpine-clang-root']
for owner in roots:
    path = owner / 'usr/lib/libz.so.1'
    if path.exists():
        payload = resolve_payload(path, owner, roots, set())
        if payload:
            shutil.copyfile(payload, root / 'usr/lib/libz.so.1')
            break
else:
    raise SystemExit('missing pinned musl libz.so.1')
PY
cp tests/fixtures/java25/Java25Probe.java "$root/tests/"
unshare --user --map-root-user --mount --pid --fork bash -s -- "$root" "$jdk" <<'SH'
set -euo pipefail
root=$1 jdk=$2
mount --bind "$jdk" "$root/opt/java25"
mount -t proc proc "$root/proc"
mount --rbind /dev "$root/dev"
chroot "$root" /opt/java25/bin/java -version
chroot "$root" /opt/java25/bin/javac -d /tests /tests/Java25Probe.java
chroot "$root" /opt/java25/bin/java -Xms32m -Xmx128m -Xint -cp /tests Java25Probe
chroot "$root" /opt/java25/bin/java -Xms32m -Xmx128m -Xbatch \
    '-XX:CompileCommand=dontinline,Java25Probe::*' -XX:+PrintCompilation \
    -cp /tests Java25Probe
SH
mkdir -p "$out/classes"
cp "$root/tests/"*.class "$out/classes/"
