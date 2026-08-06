#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/alpine-input-root}"
dev_out="${2:-.artifacts/userland-fixtures/alpine-input-dev-root}"
branch="${ALPINE_INPUT_VERSION:-v3.22}"
arch="${ALPINE_INPUT_ARCH:-x86_64}"
mirror="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
cache="${repo_root}/.artifacts/third_party/alpine-input-${branch}-${arch}"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
runtime_libc="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
cc="/usr/bin/clang"
out_abs="${repo_root}/${out}"
dev_out_abs="${repo_root}/${dev_out}"

[[ -d "${clang_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
[[ -x "${cc}" ]] || { echo "missing ${cc}" >&2; exit 1; }
[[ -e "${runtime_libc}" ]] ||
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"
mkdir -p "${cache}"
tmp="$(mktemp -d "${cache}/extract.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT
for section in main community; do
  curl -fsSL "${mirror}/${branch}/${section}/${arch}/APKINDEX.tar.gz" -o "${cache}/${section}-index.tar.gz"
  tar -xOzf "${cache}/${section}-index.tar.gz" APKINDEX >"${cache}/${section}-index"
done

field_in() {
  awk -v want="$2" -v field="$3" 'BEGIN{RS="";FS="\n"} {p=0;for(i=1;i<=NF;i++)if($i=="P:"want)p=1;if(p)for(i=1;i<=NF;i++)if(index($i,field ":")==1){print substr($i,length(field)+2);exit}}' "$1"
}
section_for() {
  for section in main community; do
    [[ -n "$(field_in "${cache}/${section}-index" "$1" V)" ]] && { printf '%s\n' "${section}"; return; }
  done
  return 1
}
field() { local section; section="$(section_for "$1")" || return 1; field_in "${cache}/${section}-index" "$1" "$2"; }
normalize() { local d="${1#!}"; printf '%s\n' "${d%%[<>=~]*}"; }
provider() {
  local dep="$1"
  case "${dep}" in so:libudev.so.1|eudev-libs|udev) printf '%s\n' libudev-zero; return;; esac
  section_for "${dep}" >/dev/null 2>&1 && { printf '%s\n' "${dep}"; return; }
  for section in main community; do
    local found
    found="$(awk -v want="${dep}" 'BEGIN{RS="";FS="\n"}{pkg="";for(i=1;i<=NF;i++)if(index($i,"P:")==1)pkg=substr($i,3);for(i=1;i<=NF;i++)if(index($i,"p:")==1){n=split(substr($i,3),a," ");for(j=1;j<=n;j++){sub(/[<>=~].*/,"",a[j]);if(a[j]==want){print pkg;exit}}}}' "${cache}/${section}-index")"
    [[ -n "${found}" ]] && { printf '%s\n' "${found}"; return; }
  done
  return 1
}

declare -A queued=() wanted=()
queue=()
enqueue() {
  local dep="$1" pkg
  case "${dep}" in ""|libc.musl-*|so:libc.musl-*|busybox|busybox-binsh|cmd:sh|/bin/sh) return;; esac
  pkg="$(provider "${dep}")" || { echo "failed to resolve Alpine input dependency ${dep}" >&2; exit 1; }
  case "${pkg}" in busybox|busybox-binsh|musl) return;; esac
  [[ -n "${queued[${pkg}]+x}" ]] || { queued["${pkg}"]=1; queue+=("${pkg}"); }
}
for pkg in seatd seatd-launch libinput libinput-libs libinput-udev libevdev mtdev libudev-zero; do enqueue "${pkg}"; done
for ((i=0; i<${#queue[@]}; i++)); do
  pkg="${queue[$i]}"; wanted["${pkg}"]=1
  for dep in $(field "${pkg}" D || true); do enqueue "$(normalize "${dep}")"; done
done

download() {
  local pkg="$1" version section apk
  version="$(field "${pkg}" V)"; section="$(section_for "${pkg}")"; apk="${cache}/${pkg}-${version}.apk"
  [[ -f "${apk}" ]] || curl -fsSL "${mirror}/${branch}/${section}/${arch}/${pkg}-${version}.apk" -o "${apk}"
  printf '%s\n' "${apk}"
}
runtime="${tmp}/runtime"; dev="${tmp}/dev"
mkdir -p "${runtime}" "${dev}"
for pkg in "${!wanted[@]}"; do tar --warning=no-unknown-keyword -xzf "$(download "${pkg}")" -C "${runtime}"; done
for pkg in libinput-dev libseat-dev libevdev-dev eudev-dev linux-headers; do
  tar --warning=no-unknown-keyword -xzf "$(download "${pkg}")" -C "${dev}"
done

# libinput parses every file in this directory for each new context. FileD's
# per-open cost makes the upstream many-file layout needlessly expensive on
# PachaOS. Preserve the exact lexical file order and contents in one parser
# input; /etc/libinput/local-overrides.quirks remains a separate final layer.
python3 - "${runtime}/usr/share/libinput" <<'PY'
import sys
from pathlib import Path

directory = Path(sys.argv[1])
sources = sorted(directory.glob("*.quirks"), key=lambda path: path.name)
if not sources:
    raise SystemExit("missing libinput quirks sources")
merged = directory / "50-pacha-merged.quirks"
payload = bytearray()
for source in sources:
    payload.extend(f"# PachaOS merged source: {source.name}\n".encode())
    contents = source.read_bytes()
    payload.extend(contents)
    if not contents.endswith(b"\n"):
        payload.extend(b"\n")
    payload.extend(b"\n")
merged.write_bytes(payload)
for source in sources:
    if source != merged:
        source.unlink()
PY
[[ "$(find "${runtime}/usr/share/libinput" -maxdepth 1 -type f -name '*.quirks' | wc -l)" == 1 ]] || {
  echo "failed to consolidate libinput quirks" >&2
  exit 1
}

# Alpine's libseat enables the logind backend and therefore carries a hard
# libelogind dependency even when LIBSEAT_BACKEND=seatd. Build the same 0.9.1
# source with only its seatd backend, which is the backend this rootfs supports.
seatd_source="${cache}/seatd-0.9.1.tar.gz"
[[ -f "${seatd_source}" ]] || curl -fsSL \
  'https://git.sr.ht/~kennylevinsen/seatd/archive/0.9.1.tar.gz' -o "${seatd_source}"
seatd_src="${tmp}/seatd-src"
mkdir -p "${seatd_src}"
tar -xzf "${seatd_source}" --strip-components=1 -C "${seatd_src}"
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" \
  -std=c11 -O2 -fPIC -shared -nostdlib -D_GNU_SOURCE -DLIBSEAT=1 -DSEATD_ENABLED=1 \
  -DSEATD_DEFAULTPATH='"/run/user/0/seatd.sock"' \
  -I"${seatd_src}" -I"${seatd_src}/include" \
  -Wl,-soname,libseat.so.1 -Wl,--version-script,"${seatd_src}/libseat/libseat.syms" \
  "${seatd_src}/common/connection.c" "${seatd_src}/common/linked_list.c" \
  "${seatd_src}/common/log.c" "${seatd_src}/libseat/backend/seatd.c" \
  "${seatd_src}/libseat/backend/noop.c" "${seatd_src}/libseat/libseat.c" \
  -o "${runtime}/usr/lib/libseat.so.1"

# The session owns one persistent seatd instance. Build the server against the
# same LPR musl ABI as the rest of the user session and bake in its private
# runtime socket instead of relying on launcher arguments or a global daemon.
"${cc}" -target x86_64-linux-musl --sysroot="${clang_root}" \
  -std=c11 -O2 -Wall -Wextra -nostdlib \
  -D_XOPEN_SOURCE=700 -D__BSD_VISIBLE -D_NETBSD_SOURCE \
  -DSEATD_VERSION='"0.9.1"' \
  -DSEATD_DEFAULTPATH='"/run/user/0/seatd.sock"' \
  -I"${dev}/usr/include" -I"${seatd_src}" -I"${seatd_src}/include" \
  "${seatd_src}/common/log.c" "${seatd_src}/common/linked_list.c" \
  "${seatd_src}/common/terminal.c" "${seatd_src}/common/connection.c" \
  "${seatd_src}/common/evdev.c" "${seatd_src}/common/hidraw.c" \
  "${seatd_src}/common/drm.c" "${seatd_src}/common/wscons.c" \
  "${seatd_src}/seatd/poller.c" "${seatd_src}/seatd/seat.c" \
  "${seatd_src}/seatd/client.c" "${seatd_src}/seatd/server.c" \
  "${seatd_src}/seatd/seatd.c" \
  "${clang_root}/usr/lib/Scrt1.o" "${clang_root}/usr/lib/crti.o" \
  "${runtime_libc}" "${clang_root}/usr/lib/crtn.o" \
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
  -o "${runtime}/usr/bin/seatd"
rm -rf "${runtime}"/.SIGN.* "${runtime}"/.PKGINFO "${runtime}"/.pre-* "${runtime}"/.post-* \
  "${runtime}"/etc/init.d "${runtime}"/etc/conf.d "${runtime}"/var \
  "${dev}"/.SIGN.* "${dev}"/.PKGINFO "${dev}"/.pre-* "${dev}"/.post-* "${dev}"/etc "${dev}"/var "${dev}"/usr/share

python3 - "${runtime}" "${clang_root}" <<'PY'
import os, sys
from pathlib import Path
root, shared_root = map(lambda p: Path(p).resolve(), sys.argv[1:])
for link in sorted((p for p in root.rglob('*') if p.is_symlink())):
    target = os.readlink(link)
    resolved = (root / target.lstrip('/')) if target.startswith('/') else (link.parent / target)
    resolved = resolved.resolve(strict=False)
    if resolved != root and root not in resolved.parents:
        raise SystemExit(f'symlink escapes root: {link} -> {target}')
    link.unlink(); link.write_bytes(b'CAPABILITYOS_ROOTFS_SYMLINK\n' + target.encode())
for path in sorted(p for p in root.rglob('*') if p.is_file()):
    relative = path.relative_to(root); shared = shared_root / relative
    if not shared.is_file(): continue
    if path.read_bytes() != shared.read_bytes():
        raise SystemExit(f'input/clang rootfs collision differs: /{relative}')
    path.unlink()
for directory in sorted((p for p in root.rglob('*') if p.is_dir()), reverse=True):
    try: directory.rmdir()
    except OSError: pass
PY

for required in usr/bin/seatd usr/bin/seatd-launch usr/lib/libseat.so.1 usr/lib/libinput.so.10 usr/lib/libudev.so.1 usr/lib/libevdev.so.2; do
  [[ -e "${runtime}/${required}" ]] || { echo "missing input runtime /${required}" >&2; exit 1; }
done
for required in usr/include/libinput.h usr/include/libseat.h usr/include/libevdev-1.0/libevdev/libevdev.h; do
  [[ -e "${dev}/${required}" ]] || { echo "missing input development /${required}" >&2; exit 1; }
done
rm -rf "${out_abs}.tmp" "${dev_out_abs}.tmp" "${out_abs}" "${dev_out_abs}"
mkdir -p "$(dirname "${out_abs}")" "$(dirname "${dev_out_abs}")"
mv "${runtime}" "${out_abs}.tmp"; mv "${dev}" "${dev_out_abs}.tmp"
mv "${out_abs}.tmp" "${out_abs}"; mv "${dev_out_abs}.tmp" "${dev_out_abs}"
printf 'built Alpine input runtime into %s\n' "${out_abs}"
printf 'runtime packages:'; printf ' %s' "${!wanted[@]}"; printf '\n'
