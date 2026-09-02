#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/alpine-xfce-root}"
branch="${ALPINE_XFCE_VERSION:-v3.22}"
arch="${ALPINE_XFCE_ARCH:-x86_64}"
mirror="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
lock="${repo_root}/tools/manifests/alpine-xfce-v3.22-x86_64.lock"
cache="${repo_root}/.artifacts/third_party/alpine-xfce-${branch}-${arch}"
clang_root="${repo_root}/.artifacts/userland-fixtures/alpine-clang-root"
mesa_root="${repo_root}/.artifacts/userland-fixtures/alpine-mesa-root"
input_root="${repo_root}/.artifacts/userland-fixtures/alpine-input-root"
linux_musl="${repo_root}/.artifacts/userland-fixtures/lpr-linux-musl-libc.so"

case "${out}" in
  /*) out_abs="${out}" ;;
  *) out_abs="${repo_root}/${out}" ;;
esac

[[ "${branch}" == "v3.22" ]] || {
  echo "the locked Xfce root requires Alpine v3.22, got ${branch}" >&2
  exit 1
}
[[ "${arch}" == "x86_64" ]] || {
  echo "the locked Xfce root is only verified for x86_64, got ${arch}" >&2
  exit 1
}
[[ -f "${lock}" ]] || { echo "missing Xfce package lock ${lock}" >&2; exit 1; }
[[ -d "${clang_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_clang.sh"
[[ -d "${mesa_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_mesa.sh"
[[ -d "${input_root}" ]] || bash "${repo_root}/tools/build_wsl_alpine_input.sh"
[[ -f "${linux_musl}" ]] ||
  bash "${repo_root}/tools/copy_lpr_linux_musl.sh" \
    ".artifacts/userland-fixtures/lpr-linux-musl-libc.so"

require_locked() {
  local package="$1" version="$2"
  awk -v package="${package}" -v version="${version}" '
    $1 !~ /^#/ && $2 == package && $3 == version { found=1 }
    END { exit found ? 0 : 1 }
  ' "${lock}" || {
    echo "Xfce lock does not contain ${package}-${version}" >&2
    exit 1
  }
}

require_locked xorg-server 21.1.19-r0
require_locked xf86-input-libinput 1.5.0-r0
require_locked bash 5.2.37-r0
require_locked readline 8.2.13-r1
require_locked alpine-keys 2.5-r0
require_locked apk-tools 2.14.10-r0
require_locked libapk2 2.14.10-r0
require_locked musl 1.2.5-r12
require_locked busybox 1.37.0-r20
require_locked busybox-binsh 1.37.0-r20
require_locked xfce4 4.20-r0
require_locked xfce4-session 4.20.2-r0
require_locked xfwm4 4.20.0-r1
require_locked xfce4-panel 4.20.4-r0
require_locked xfdesktop 4.20.1-r0
require_locked thunar 4.20.3-r0
require_locked xfce4-terminal 1.1.3-r0
require_locked xfce4-notifyd 0.9.6-r0
require_locked gtk+3.0-demo 3.24.50-r0
require_locked json-c 0.18-r1

if awk '$1 !~ /^#/ && $2 == "polkit-elogind-libs" { found=1 } END { exit found ? 0 : 1 }' "${lock}"; then
  echo "Xfce lock must use polkit-noelogind-libs" >&2
  exit 1
fi

mkdir -p "${cache}"
tmp="$(mktemp -d "${cache}/extract.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT
runtime="${tmp}/runtime"
mkdir -p "${runtime}"
package_count=0
package_apks=()

while read -r section package version expected_sha256 extra; do
  [[ -n "${section}" && "${section}" != \#* ]] || continue
  [[ -z "${extra:-}" ]] || {
    echo "invalid Xfce lock entry for ${package}" >&2
    exit 1
  }
  case "${section}" in main|community) ;; *)
    echo "invalid Alpine section in Xfce lock: ${section}" >&2
    exit 1
  esac
  [[ "${package}" =~ ^[A-Za-z0-9_.+:-]+$ && "${version}" =~ ^[A-Za-z0-9_.+~-]+$ && "${expected_sha256}" =~ ^[0-9a-f]{64}$ ]] || {
    echo "invalid Xfce lock fields: ${section} ${package} ${version}" >&2
    exit 1
  }
  apk="${cache}/${package}-${version}.apk"
  actual_sha256=""
  if [[ -f "${apk}" ]]; then
    actual_sha256="$(sha256sum "${apk}" | awk '{print $1}')"
  fi
  if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
    staged="${tmp}/${package}-${version}.apk.download"
    curl -fsSL \
      "${mirror}/${branch}/${section}/${arch}/${package}-${version}.apk" \
      -o "${staged}"
    actual_sha256="$(sha256sum "${staged}" | awk '{print $1}')"
    if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
      echo "locked Alpine APK checksum mismatch: ${package}-${version}" >&2
      echo "expected ${expected_sha256}, got ${actual_sha256}" >&2
      exit 1
    fi
    mv -f "${staged}" "${apk}"
  fi
  pkginfo="$(tar --warning=no-unknown-keyword -xOzf "${apk}" .PKGINFO)"
  grep -Fxq "pkgname = ${package}" <<<"${pkginfo}" || {
    echo "locked Alpine APK package mismatch: ${package}" >&2
    exit 1
  }
  grep -Fxq "pkgver = ${version}" <<<"${pkginfo}" || {
    echo "locked Alpine APK version mismatch: ${package}-${version}" >&2
    exit 1
  }
  tar --warning=no-unknown-keyword -xzf "${apk}" -C "${runtime}"
  package_apks+=("${apk}")
  package_count=$((package_count + 1))
done <"${lock}"

locked_count="$(awk '$1 == "#" && $2 == "package-count" { print $3 }' "${lock}")"
[[ -n "${locked_count}" && "${package_count}" == "${locked_count}" ]] || {
  echo "Xfce package count mismatch: lock=${locked_count:-missing} extracted=${package_count}" >&2
  exit 1
}

# Register the exact locked package set in Alpine's native installed database.
# Extraction alone provides the files but leaves apk unable to distinguish the
# shipped Xfce stack from unmanaged files.  Running the target apk without
# package scripts keeps this build deterministic while retaining upstream
# package metadata, dependency edges, and file ownership records.
command -v fakeroot >/dev/null 2>&1 || {
  echo "fakeroot is required to construct the Xfce apk database" >&2
  exit 1
}
apk_bootstrap_libraries="${tmp}/apk-bootstrap-libraries"
python3 "${repo_root}/tools/rootfs_overlay.py" library-view \
  "${apk_bootstrap_libraries}" \
  "${runtime}" "${mesa_root}" "${input_root}" "${clang_root}"
fakeroot -- "${linux_musl}" --library-path "${apk_bootstrap_libraries}" \
  "${runtime}/sbin/apk" \
  --root "${runtime}" \
  --initdb \
  --no-cache \
  --no-network \
  --no-progress \
  --no-scripts \
  add "${package_apks[@]}" >"${tmp}/apk-install.log"

LC_ALL=C awk '$1 !~ /^#/ { print $2 " " $3 }' "${lock}" |
  sort >"${tmp}/locked-packages"
LC_ALL=C awk '
  /^P:/ { package=substr($0, 3); next }
  /^V:/ && package != "" { print package " " substr($0, 3); package="" }
' "${runtime}/lib/apk/db/installed" | sort >"${tmp}/installed-packages"
if ! cmp -s "${tmp}/locked-packages" "${tmp}/installed-packages"; then
  diff -u "${tmp}/locked-packages" "${tmp}/installed-packages" >&2 || true
  echo "Xfce apk installed database differs from the package lock" >&2
  exit 1
fi

# apk's build-time transaction leaves its process lock behind.  A lock is
# runtime state, not package database content; shipping it also makes the first
# guest transaction depend on the build host's file mode and ownership.  Let
# the guest apk create and own the lock for each transaction instead.
rm -f "${runtime}/lib/apk/db/lock"

mkdir -p "${runtime}/etc/apk"
printf '%s\n' "${arch}" >"${runtime}/etc/apk/arch"
printf '%s\n' \
  "${mirror}/${branch}/main" \
  "${mirror}/${branch}/community" \
  >"${runtime}/etc/apk/repositories"
awk '$1 !~ /^#/ { print $2 "=" $3 }' "${lock}" \
  >"${runtime}/etc/apk/world"

rm -rf \
  "${runtime}"/.SIGN.* "${runtime}"/.PKGINFO \
  "${runtime}"/.pre-* "${runtime}"/.post-* \
  "${runtime}"/etc/init.d "${runtime}"/etc/conf.d \
  "${runtime}"/var/cache/apk
mkdir -p "${runtime}/usr/share/pacha"
cp "${lock}" "${runtime}/usr/share/pacha/xfce-packages.lock"

# pack.yaml publishes the project-wide runtime loader, libc, /bin/sh, and CA
# bundle at these exact paths. Keep their Alpine packages in the installed
# database for dependency completeness, but leave path ownership to those
# canonical artifacts.
rm -f \
  "${runtime}/bin/sh" \
  "${runtime}/lib/ld-musl-x86_64.so.1" \
  "${runtime}/lib/libc.musl-x86_64.so.1" \
  "${runtime}/etc/ssl/cert.pem" \
  "${runtime}/etc/ssl/certs/ca-certificates.crt" \
  "${runtime}/usr/share/ca-certificates/mozilla/NetLock_Arany_=Class_Gold=_Főtanúsítvány.crt"

# apk is run without package scripts, so install the standard BusyBox entry
# points used by Alpine's init and Xorg's upstream startx script explicitly.
# These are ordinary rootfs policy; seed0root only needs to execute /sbin/init.
ln -s ../bin/busybox "${runtime}/sbin/init"
ln -s busybox "${runtime}/bin/sed"
ln -s busybox "${runtime}/bin/hostname"

install -d -m 0700 "${runtime}/root"
printf '%s\n' \
  '#!/bin/sh' \
  'exec /usr/bin/dbus-run-session -- /usr/bin/startxfce4' \
  >"${runtime}/root/.xinitrc"
chmod 0755 "${runtime}/root/.xinitrc"

printf '%s\n' \
  '#!/bin/sh' \
  'profile=$(/bin/busybox cat /etc/pacha_boot_profile 2>/dev/null || true)' \
  'case "${profile}" in' \
  '  *console-shell*)' \
  '    exec /bin/busybox env -i HOME=/root USER=root LOGNAME=root SHELL=/bin/bash PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/cmd TERM=linux LANG=C.UTF-8 LC_ALL=C.UTF-8 /bin/bash --noprofile --norc -i' \
  '    ;;' \
  'esac' \
  'exec /bin/busybox env -i HOME=/root USER=root LOGNAME=root SHELL=/bin/bash PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/cmd TERM=linux XDG_RUNTIME_DIR=/run/user/0 LANG=C.UTF-8 LC_ALL=C.UTF-8 /usr/bin/startx /root/.xinitrc' \
  >"${runtime}/sbin/pacha-boot-session"
chmod 0755 "${runtime}/sbin/pacha-boot-session"

printf '%s\n' \
  '::sysinit:/bin/busybox mkdir -p /run/user/0' \
  '::sysinit:/bin/busybox chmod 0700 /run/user/0' \
  '::respawn:/sbin/pacha-boot-session' \
  >"${runtime}/etc/inittab"

# startx registers both :0 and $(uname -n):0 in Xauthority. Keep the Linux
# runtime's nodename resolvable through the standard files NSS source.
printf '%s\n' \
  '127.0.0.1 localhost localhost.localdomain pachaos' \
  '::1 localhost localhost.localdomain pachaos' \
  >"${runtime}/etc/hosts"

# Recreate the caches that Alpine APK triggers normally generate. The target
# binaries run through the same musl loader ABI that PachaOS will use.
mkdir -p "${runtime}/var/cache/fontconfig"
env -u SOURCE_DATE_EPOCH /usr/bin/fc-cache \
  --really-force --system-only --sysroot="${runtime}" \
  /usr/share/fonts >/dev/null
XDG_DATA_DIRS="${runtime}/usr/share" \
  /usr/bin/update-mime-database "${runtime}/usr/share/mime" >/dev/null

target_library_path="${runtime}/lib:${runtime}/usr/lib:${clang_root}/lib:${clang_root}/usr/lib:${mesa_root}/lib:${mesa_root}/usr/lib:${input_root}/lib:${input_root}/usr/lib"
schema_dir="${runtime}/usr/share/glib-2.0/schemas"
if [[ -d "${schema_dir}" ]]; then
  "${linux_musl}" --library-path "${target_library_path}" \
    "${runtime}/usr/bin/glib-compile-schemas" "${schema_dir}"
fi
for icon_dir in "${runtime}"/usr/share/icons/*; do
  [[ -f "${icon_dir}/index.theme" ]] || continue
  "${linux_musl}" --library-path "${target_library_path}" \
    "${runtime}/usr/bin/gtk-update-icon-cache" -f -q "${icon_dir}"
done
"${linux_musl}" --library-path "${target_library_path}" \
  "${runtime}/usr/bin/update-desktop-database" \
  "${runtime}/usr/share/applications"

pixbuf_dir="${runtime}/usr/lib/gdk-pixbuf-2.0/2.10.0"
GDK_PIXBUF_MODULEDIR="${pixbuf_dir}/loaders" \
  "${linux_musl}" --library-path "${target_library_path}" \
  "${runtime}/usr/bin/gdk-pixbuf-query-loaders" |
  /usr/bin/sed "s#${runtime}##g" >"${pixbuf_dir}/loaders.cache"
[[ -s "${pixbuf_dir}/loaders.cache" ]] || {
  echo "target Xfce gdk-pixbuf loader cache is empty" >&2
  exit 1
}

library_root="${tmp}/loader-libraries"
python3 "${repo_root}/tools/rootfs_overlay.py" library-view \
  "${library_root}" "${runtime}" "${mesa_root}" "${input_root}" "${clang_root}"

for executable in \
  bin/bash \
  sbin/apk \
  usr/libexec/Xorg \
  usr/bin/xfwm4 \
  usr/bin/xfce4-session \
  usr/bin/xfce4-panel \
  usr/bin/xfdesktop \
  usr/bin/xfsettingsd \
  usr/bin/xfce4-about \
  usr/bin/gtk3-demo \
  usr/bin/thunar \
  usr/bin/xfce4-terminal \
  usr/bin/xprop \
  usr/bin/xwininfo \
  usr/bin/dbus-daemon; do
  report="${tmp}/$(basename "${executable}").loader"
  if ! "${linux_musl}" --library-path "${library_root}" --list \
      "${runtime}/${executable}" >"${report}" 2>&1; then
    cat "${report}" >&2
    echo "Xfce executable does not resolve: /${executable}" >&2
    exit 1
  fi
  if grep -Fq 'not found' "${report}"; then
    cat "${report}" >&2
    echo "Xfce executable has an unresolved library: /${executable}" >&2
    exit 1
  fi
done

for required in \
  bin/bash \
  bin/busybox \
  bin/sed \
  bin/hostname \
  sbin/init \
  sbin/apk \
  usr/lib/libreadline.so.8 \
  usr/lib/libapk.so.2.14.10 \
  usr/lib/libncursesw.so.6 \
  usr/bin/Xorg \
  usr/libexec/Xorg \
  usr/bin/xinit \
  usr/bin/startxfce4 \
  usr/lib/xorg/modules/drivers/modesetting_drv.so \
  usr/lib/xorg/modules/input/libinput_drv.so \
  usr/bin/xauth \
  usr/bin/dbus-uuidgen \
  usr/bin/dbus-run-session \
  usr/bin/xfce4-session \
  usr/bin/xfwm4 \
  usr/bin/xfce4-panel \
  usr/bin/xfdesktop \
  usr/bin/xfsettingsd \
  usr/bin/xfce4-about \
  usr/bin/gtk3-demo \
  usr/bin/thunar \
  usr/bin/xfce4-terminal \
  usr/bin/xfce4-notifyd-config \
  usr/bin/xprop \
  usr/bin/xwininfo \
  usr/lib/xfce4/notifyd/xfce4-notifyd \
  usr/share/xsessions/xfce.desktop \
  usr/share/X11/xkb/rules/evdev \
  usr/share/themes/Default/xfwm4/themerc \
  usr/share/icons/Adwaita/index.theme \
  usr/share/icons/hicolor/index.theme \
  usr/share/fonts/ipaexfont/ipaexg.ttf \
  usr/share/fonts/roboto-mono/RobotoMono\[wght\].ttf \
  usr/share/dbus-1/services/org.xfce.xfce4-notifyd.Notifications.service \
  etc/apk/arch \
  etc/apk/repositories \
  etc/apk/world \
  etc/apk/keys/alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub \
  lib/apk/db/installed \
  etc/xdg/xfce4/helpers.rc \
  etc/X11/xinit/xserverrc \
  etc/xdg/xfce4/xinitrc \
  etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-session.xml \
  etc/xdg/autostart/xfsettingsd.desktop \
  etc/inittab \
  root/.xinitrc \
  sbin/pacha-boot-session \
  usr/share/pacha/xfce-packages.lock; do
  if [[ ! -e "${runtime}/${required}" && ! -e "${mesa_root}/${required}" && ! -e "${input_root}/${required}" && ! -e "${clang_root}/${required}" ]]; then
    echo "missing Xfce runtime /${required}" >&2
    exit 1
  fi
done
[[ -s "${runtime}/usr/share/mime/mime.cache" ]] || {
  echo "missing Xfce MIME cache" >&2
  exit 1
}

python3 "${repo_root}/tools/rootfs_overlay.py" dedupe \
  "${runtime}" "${clang_root}" "${mesa_root}" "${input_root}"

# Xorg's packaged default log path is /var/log/Xorg.0.log.  Alpine normally
# gets this FHS directory from its base filesystem/init setup, while this
# package-only rootfs has no init package to create an otherwise empty path.
install -d -m 0755 "${runtime}/var/log"
install -d -m 0700 "${runtime}/run/user/0"

rm -rf "${out_abs}.tmp" "${out_abs}"
mkdir -p "$(dirname "${out_abs}")"
mv "${runtime}" "${out_abs}.tmp"
mv "${out_abs}.tmp" "${out_abs}"

printf 'built locked Alpine Xfce runtime into %s\n' "${out_abs}"
printf 'locked packages: %s\n' "${package_count}"
