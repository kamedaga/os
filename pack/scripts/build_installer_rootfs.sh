#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output="${1:-$repo_root/.artifacts/installer-rootfs/pachaos-alpine-rootfs.tar}"
debug_root="${2:-}"
cache="$repo_root/.artifacts/third_party/alpine-minirootfs-3.22.5-x86_64.tar.gz"
stage="$repo_root/.artifacts/installer-rootfs/stage"
root_sha=4b4daa9fe2fc696c4919c4412a4c3d3e770d8fb70292a004a2c72f5096175282
url="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}/v3.22/releases/x86_64/alpine-minirootfs-3.22.5-x86_64.tar.gz"

mkdir -p "$(dirname "$cache")" "$(dirname "$output")"
if [[ ! -f "$cache" ]] || ! echo "$root_sha  $cache" | sha256sum -c --status; then
    curl -fL --retry 3 "$url" -o "$cache.download"
    echo "$root_sha  $cache.download" | sha256sum -c
    mv "$cache.download" "$cache"
fi

# The installer root is a fresh official Alpine base. The ordinary developer
# rootfs is accepted only as an explicit second argument and produces a
# separate debug artifact; it is never copied implicitly.
rm -rf "$stage"
mkdir -p "$stage"
tar -xzf "$cache" -C "$stage"
if [[ -n "$debug_root" ]]; then
    [[ -d "$debug_root" ]] || { echo "debug root is not a directory: $debug_root" >&2; exit 1; }
    cp -a "$debug_root/." "$stage/"
fi

# Alpine's base group file reserves the kvm group with a member named "kvm",
# but the minirootfs intentionally has no matching passwd entry.  Linux libc
# tolerates that dangling membership; PachaOS rejects it because account names
# are later turned into process credentials.  Preserve the group and remove
# only the nonexistent membership in the installed image.
if grep -qxF 'kvm:x:34:kvm' "$stage/etc/group"; then
    sed -i 's/^kvm:x:34:kvm$/kvm:x:34:/' "$stage/etc/group"
fi

install_artifact() {
    local app="$1" artifact="$2" destination="$3"
    local source="$repo_root/.artifacts/userland/$app/$artifact"
    [[ -s "$source" ]] || {
        echo "missing $source; build userland before installer rootfs" >&2
        exit 1
    }
    install -D -m 0755 "$source" "$stage$destination"
}

install_runtime_artifact() {
    local app="$1" artifact="$2" destination="$3" mode="${4:-0755}"
    local source="$repo_root/.artifacts/userland/$app/$artifact"
    [[ -s "$source" ]] || {
        echo "missing $source; build userland before installer rootfs" >&2
        exit 1
    }
    mkdir -p "$(dirname "$stage$destination")"
    # Alpine provides its own loader, and libc.musl-x86_64.so.1 is normally a
    # symlink to it.  Installed PachaOS services are native binaries while LPR
    # guests need a separate Linux musl.  Replace each exact staging entry so
    # install(1) cannot follow Alpine's symlink and overwrite the wrong file.
    rm -f "$stage$destination"
    install -m "$mode" "$source" "$stage$destination"
}

install_artifact filed FILED.ELF /sbin/filed.elf
install_artifact seed0root SEED0RT.ELF /sbin/seed0root.elf
install_artifact lpr_supervisor LPRSUP.ELF /sbin/lpr_supervisor.elf
install_artifact unixd UNIXD.ELF /srv/unixd.elf
install_artifact termd TERMD.ELF /srv/termd.elf
install_artifact inputd INPUTD.ELF /srv/inputd.elf
install_artifact usbd USBD.ELF /srv/usbd.elf
install_artifact netd NETD.ELF /srv/netd.elf
install_artifact logd LOGD.ELF /srv/logd.elf
install_artifact linux_lpr_runtime LPRLINUX.SO /lib/pacha/lpr-linux-x86_64.so

# termd keeps the Linux TTY implementation in its existing user-space island.
# These are runtime dependencies of the installed service, not optional build
# products; omitting them makes an otherwise valid ext4 installation fail
# before it can provide a console.
install_artifact kobox_linux_virtio LXVIRT.KO /usr/lib/kobox/linux_virtio.ko
install_artifact kobox_linux_virtio_ring LXVTRNG.KO /usr/lib/kobox/linux_virtio_ring.ko
install_artifact kobox_linux_virtio_pci_modern_dev LXVMODN.KO /usr/lib/kobox/linux_virtio_pci_modern_dev.ko
install_artifact kobox_linux_virtio_pci_legacy_dev LXVLEGD.KO /usr/lib/kobox/linux_virtio_pci_legacy_dev.ko
install_artifact kobox_linux_virtio_pci LXVPCI.KO /usr/lib/kobox/linux_virtio_pci.ko
install_artifact kobox_linux_tty_core TTYCORE.KO /usr/lib/kobox/linux_tty_core.ko
install_artifact kobox_linux_tty_n_null TTYNULL.KO /usr/lib/kobox/linux_tty_n_null.ko
install_artifact kobox_linux_virtio_console TTYVCON.KO /usr/lib/kobox/linux_virtio_console.ko

# Keep the two ABIs explicit.  Native services must start through the PachaOS
# musl loader; ordinary Alpine/Linux programs launched by LPR must continue to
# see an unmodified Linux musl loader and libc under the established paths.
install_runtime_artifact ld_musl_path LDMUSL.PATH /etc/ld-musl-x86_64.path 0644
install_runtime_artifact pachaos_musl_ldso PAMUSLLD.SO /usr/lib/pachaos/ld-musl-x86_64.so.1
install_runtime_artifact pachaos_musl_libc PAMUSLLC.SO /usr/lib/pachaos/libc.so
install_runtime_artifact lpr_linux_musl_libc LPRMUSL.SO /lib/libc.musl-x86_64.so.1
install_runtime_artifact lpr_linux_musl_libc LPRMUSL.SO /lib/linux/ld-musl-x86_64.so.1
install_runtime_artifact lpr_linux_musl_libc LPRMUSL.SO /lib/linux/libc.so

# The compact installed image is intentionally not the developer desktop
# root.  This profile makes that distinction explicit to seed0root, while the
# BusyBox init policy provides the initial Linux ash on termd's existing PTY.
install -D -m 0644 "$repo_root/pack/installer-rootfs/pacha_boot_profile" \
    "$stage/etc/pacha_boot_profile"
install -D -m 0644 "$repo_root/pack/installer-rootfs/inittab" \
    "$stage/etc/inittab"

install_kobox_package() {
    local source="$1" destination="$2"
    [[ -d "$source" ]] || {
        echo "missing Kobox2 package $source; build block/network packages first" >&2
        exit 1
    }
    # Production build directories also contain linker scratch trees,
    # inventories and optional diagnostic runtimes.  They are build evidence,
    # not the installed Kobox2 package.  Besides wasting substantial space,
    # copying those trees leaks host-derived object names into the target and
    # can exceed the guest VFS component limit.  Keep the installed contract
    # identical to the curated live-boot package: launcher, runtime, manifest
    # and loadable modules only.
    for required in sandbox.elf core.so manifest.bin modules; do
        [[ -e "$source/$required" ]] || {
            echo "incomplete Kobox2 package $source: missing $required" >&2
            exit 1
        }
    done
    install -D -m 0755 "$source/sandbox.elf" "$stage$destination/sandbox.elf"
    install -D -m 0755 "$source/core.so" "$stage$destination/core.so"
    install -D -m 0644 "$source/manifest.bin" "$stage$destination/manifest.bin"
    mkdir -p "$stage$destination/modules"
    cp -a "$source/modules/." "$stage$destination/modules/"
}
install_kobox_package "$repo_root/.artifacts/kobox2-network-production" \
    /srv/kobox2/network
install_kobox_package "$repo_root/.artifacts/kobox2-usb-storage-xhci-production" \
    /srv/kobox2/usb-storage
install_kobox_package "$repo_root/.artifacts/kobox2-nvme-production" \
    /srv/kobox2/nvme

mkdir -p "$stage/dev/dri" "$stage/dev/input" "$stage/dev/shm" \
    "$stage/proc" "$stage/run" "$stage/sys" "$stage/tmp"
chmod 1777 "$stage/tmp" "$stage/dev/shm"
printf 'PachaOS Alpine base (installed)\n' > "$stage/etc/pacha-release"

tar --numeric-owner --owner=0 --group=0 -C "$stage" -cf "$output" .
# Build-only Kobox2 trees must never re-enter the installer payload through a
# future change to the package layout.
if tar -tf "$output" | grep -Eq '/(runtime|runtime-frame-diagnostic|\.metadata|\.objects|\.module-exports)(/|$)'; then
    echo "installer rootfs contains Kobox2 build-only files" >&2
    exit 1
fi
sha256sum "$output" > "$output.sha256"
printf '%s\n' "$output"
