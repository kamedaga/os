#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
xfce_root="${repo_root}/.artifacts/userland-fixtures/alpine-xfce-root"
lock="${repo_root}/tools/manifests/alpine-xfce-v3.22-x86_64.lock"
manifest="${repo_root}/.artifacts/manifests/rootfs.generated.txt"
linux_musl="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
input_root="${repo_root}/.artifacts/userland-fixtures/alpine-input-root"
mesa_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-root"
pine2_root="${repo_root}/.artifacts/userland-fixtures/pine2-gtk-root"

cd "${repo_root}"
bash tools/build_wsl_alpine_xfce.sh

package_count="$(awk '$1 !~ /^#/ { count++ } END { print count + 0 }' "${lock}")"
locked_count="$(awk '$1 == "#" && $2 == "package-count" { print $3 }' "${lock}")"
[[ -n "${locked_count}" && "${package_count}" == "${locked_count}" ]] || {
  echo "Xfce package lock count mismatch: metadata=${locked_count:-missing} entries=${package_count}" >&2
  exit 1
}
cmp "${lock}" "${xfce_root}/usr/share/pacha/xfce-packages.lock"

for required in \
  bin/busybox \
  bin/sed \
  bin/hostname \
  sbin/init \
  sbin/apk \
  usr/lib/libapk.so.2.14.10 \
  etc/apk/arch \
  etc/apk/repositories \
  etc/apk/world \
  etc/hosts \
  etc/inittab \
  root/.xinitrc \
  etc/apk/keys/alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub \
  lib/apk/db/installed \
  usr/libexec/Xorg \
  usr/bin/xinit \
  usr/bin/startxfce4 \
  usr/lib/xorg/modules/drivers/modesetting_drv.so \
  usr/lib/xorg/modules/input/libinput_drv.so \
  usr/bin/dbus-uuidgen \
  usr/bin/xfce4-session \
  usr/bin/xfwm4 \
  usr/bin/xfce4-panel \
  usr/bin/xfdesktop \
  usr/bin/xfsettingsd \
  usr/bin/xfce4-about \
  usr/bin/gtk3-demo \
  usr/bin/thunar \
  usr/bin/xfce4-terminal \
  usr/lib/xfce4/notifyd/xfce4-notifyd \
  etc/X11/xinit/xserverrc \
  etc/xdg/xfce4/xinitrc \
  etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-session.xml \
  etc/xdg/autostart/xfsettingsd.desktop \
  usr/share/themes/Default/xfwm4/themerc \
  usr/share/X11/xkb/rules/evdev; do
  [[ -e "${xfce_root}/${required}" ]] || {
    echo "missing Phase 1 Xfce file: /${required}" >&2
    exit 1
  }
done

[[ -d "${xfce_root}/var/log" ]] || {
  echo "missing standard Xorg log directory: /var/log" >&2
  exit 1
}
[[ -d "${xfce_root}/run/user/0" ]] || {
  echo "missing XDG runtime directory seed: /run/user/0" >&2
  exit 1
}
[[ "$(stat -c '%a' "${xfce_root}/run/user/0")" == 700 ]] || {
  echo "XDG runtime directory seed must have mode 0700" >&2
  exit 1
}

grep -Fxq 'x86_64' "${xfce_root}/etc/apk/arch"
grep -Fxq 'https://dl-cdn.alpinelinux.org/alpine/v3.22/main' \
  "${xfce_root}/etc/apk/repositories"
grep -Fxq 'https://dl-cdn.alpinelinux.org/alpine/v3.22/community' \
  "${xfce_root}/etc/apk/repositories"
grep -Fxq 'apk-tools=2.14.10-r0' "${xfce_root}/etc/apk/world"
grep -Fxq 'xfce4=4.20-r0' "${xfce_root}/etc/apk/world"
grep -Fxq '127.0.0.1 localhost localhost.localdomain pachaos' \
  "${xfce_root}/etc/hosts"
grep -Fxq '::1 localhost localhost.localdomain pachaos' \
  "${xfce_root}/etc/hosts"
[[ "$(rg -c '^P:' "${xfce_root}/lib/apk/db/installed")" == "${package_count}" ]] || {
  echo "Xfce apk installed database does not match the lock count" >&2
  exit 1
}
[[ ! -e "${xfce_root}/lib/apk/db/lock" ]] || {
  echo "Xfce rootfs must not ship apk's runtime database lock" >&2
  exit 1
}
apk_library_tmp="$(mktemp -d)"
apk_library_view="${apk_library_tmp}/libraries"
trap 'rm -rf "${apk_library_tmp}"' EXIT
python3 tools/rootfs_overlay.py library-view \
  "${apk_library_view}" \
  "${xfce_root}" "${mesa_root}" "${input_root}" "${clang_root}" "${pine2_root}"
"${linux_musl}" --library-path "${apk_library_view}" \
  "${xfce_root}/sbin/apk" --version | grep -Fq 'apk-tools 2.14.10'
for installed_package in apk-tools alpine-keys xfce4 xfwm4; do
  "${linux_musl}" --library-path "${apk_library_view}" \
    "${xfce_root}/sbin/apk" --root "${xfce_root}" \
    info --installed "${installed_package}" >/dev/null
done
pine2_loader_report="${apk_library_tmp}/pine2-gtk.loader"
if ! "${linux_musl}" --library-path "${apk_library_view}" \
    --list "${pine2_root}/usr/bin/pine2-gtk" >"${pine2_loader_report}" 2>&1; then
  cat "${pine2_loader_report}" >&2
  echo "Pine2 GTK does not resolve against the final Xfce rootfs libraries" >&2
  exit 1
fi
if grep -Fq 'not found' "${pine2_loader_report}"; then
  cat "${pine2_loader_report}" >&2
  echo "Pine2 GTK has an unresolved final-rootfs library" >&2
  exit 1
fi

for required_quirk in 10-generic-keyboard.quirks 30-vendor-qemu.quirks; do
  [[ -s "${input_root}/usr/share/libinput/${required_quirk}" ]] || {
    echo "missing upstream libinput quirk: /usr/share/libinput/${required_quirk}" >&2
    exit 1
  }
done
[[ ! -e "${input_root}/usr/share/libinput/50-pacha-merged.quirks" ]] || {
  echo "input root unexpectedly contains a duplicate merged quirks database" >&2
  exit 1
}

PACGO_PROGRESS=plain ./pacgo gen manifests
grep -Fxq '/var/log=@dir' "${manifest}"
grep -Fxq '/run=@dir' "${manifest}"
grep -Fq '/sbin/init=' "${manifest}"
grep -Fq '/etc/inittab=' "${manifest}"
grep -Fq '/etc/hosts=' "${manifest}"
grep -Fq '/root/.xinitrc=' "${manifest}"
grep -Fq '/usr/bin/dbus-run-session=' "${manifest}"
grep -Fq '/usr/bin/startxfce4=' "${manifest}"
grep -Fq '/usr/libexec/Xorg=' "${manifest}"
grep -Fq '/usr/bin/xfwm4=' "${manifest}"
grep -Fq '/usr/bin/xfce4-panel=' "${manifest}"
grep -Fq '/usr/bin/xfdesktop=' "${manifest}"
grep -Fq '/usr/bin/xfce4-about=' "${manifest}"
grep -Fq '/usr/bin/gtk3-demo=' "${manifest}"
grep -Fq '/usr/bin/pine2-gtk=' "${manifest}"
grep -Fq '/sbin/apk=' "${manifest}"
grep -Fq '/etc/apk/repositories=' "${manifest}"
grep -Fq '/lib/apk/db/installed=' "${manifest}"
if grep -Fq '/lib/apk/db/lock=' "${manifest}"; then
  echo "Xfce rootfs manifest ships apk's runtime database lock" >&2
  exit 1
fi
[[ "$(grep -Fc '/usr/share/libinput/30-vendor-qemu.quirks=' "${manifest}")" == 1 ]] || {
  echo "rootfs manifest does not have exactly one canonical libinput quirks dataset" >&2
  exit 1
}
if grep -Fq '/usr/share/libinput/50-pacha-merged.quirks=' "${manifest}"; then
  echo "rootfs manifest contains a duplicate merged libinput quirks database" >&2
  exit 1
fi
if grep -Fq '/usr/bin/sway=' "${manifest}"; then
  echo "Sway unexpectedly remains in the default Xfce rootfs manifest" >&2
  exit 1
fi
if grep -Fq '/usr/libexec/pacha-user-session=' "${manifest}"; then
  echo "legacy Pacha-specific desktop session remains in the Xfce rootfs" >&2
  exit 1
fi
if find "${xfce_root}" -name '*=*' -print -quit | grep -q .; then
  echo "Xfce rootfs contains a path that conflicts with the manifest delimiter" >&2
  exit 1
fi

if rg -q 'pacha-xfce-session|PACHAXFS' \
    userland/seed0root/src/main.c pack/pack.yaml; then
  echo "Pacha-specific Xfce launcher remains wired into the boot path" >&2
  exit 1
fi
if rg -q 'dbus-run-session|startxfce4|xfce4-session|HOME=/home|XDG_RUNTIME_DIR' \
    userland/seed0root/src/main.c; then
  echo "seed0root still contains desktop-session policy" >&2
  exit 1
fi
grep -Fxq 'exec /usr/bin/dbus-run-session -- /usr/bin/startxfce4' \
  "${xfce_root}/root/.xinitrc"
grep -Fxq '::respawn:/sbin/pacha-boot-session' "${xfce_root}/etc/inittab"
grep -Fq '/etc/pacha_boot_profile' "${xfce_root}/sbin/pacha-boot-session"
grep -Fq 'console-shell' "${xfce_root}/sbin/pacha-boot-session"
grep -Fq '/bin/bash --noprofile --norc -i' "${xfce_root}/sbin/pacha-boot-session"
grep -Fq '/usr/bin/startx /root/.xinitrc' "${xfce_root}/sbin/pacha-boot-session"

printf 'XFCE_PHASE1_CHECK=PASS packages=%s root=%s route=init-boot-session-startx-startxfce4\n' \
  "${package_count}" "${xfce_root}"
