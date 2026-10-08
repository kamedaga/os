#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "$repo_root/.artifacts/live-desktop-import-test.XXXXXX")"
echo "Live desktop import evidence: $work"
mkdir -p "$work/payload/usr/share/pacha" "$work/payload/usr/libexec" \
  "$work/payload/usr/bin" "$work/payload/sbin" "$work/root/etc/pacha"
for command in usr/libexec/Xorg usr/bin/startx usr/bin/startxfce4 sbin/apk usr/bin/gtk3-demo usr/bin/dbus-uuidgen; do
    cp /usr/bin/true "$work/payload/$command"
done
cp /usr/bin/dbus-uuidgen "$work/payload/usr/bin/dbus-uuidgen"
ln -s startx "$work/payload/usr/bin/desktop-alias"
(cd "$work/payload" && sha256sum usr/libexec/Xorg usr/bin/startx \
  usr/bin/startxfce4 sbin/apk usr/bin/gtk3-demo usr/bin/dbus-uuidgen >usr/share/pacha/live-desktop-files.sha256)
printf 'usr/bin/desktop-alias\tstartx\n' >"$work/payload/usr/share/pacha/live-desktop-links"
tar -C "$work/payload" -czf "$work/DESKTOP.TGZ" .
(cd "$work" && sha256sum DESKTOP.TGZ >DESKTOP.SHA)
printf 'ram-tmpfs-v1\n' >"$work/root/etc/pacha/boot-profile"
offset=$((1024 * 1024))
truncate -s 72M "$work/media.img"
image="$work/media.img@@$offset"
mformat -i "$image" -T 131072 -F ::
mcopy -i "$image" "$work/DESKTOP.TGZ" ::/DESKTOP.TGZ
mcopy -i "$image" "$work/DESKTOP.SHA" ::/DESKTOP.SHA
before="$(sha256sum "$work/media.img")"
PACHA_LIVE_ROOT="$work/root" busybox ash \
  "$repo_root/pack/installer-bootfs/pacha-live-import" "$work/media.img" "$offset"
test -f "$work/root/run/pacha/live-desktop.ready"
test ! -f "$work/root/run/pacha/DESKTOP.TGZ"
test ! -f "$work/root/run/pacha/live-desktop.sha256"
test "$(readlink "$work/root/usr/bin/desktop-alias")" = startx
test "$(stat -c %a "$work/root/run/user/0")" = 700
test "$(wc -c <"$work/root/etc/machine-id")" = 33
for applet in id mkdir cat cp rm; do
    test -L "$work/root/bin/$applet"
    test -x "$work/root/bin/$applet"
    case "$(readlink "$work/root/bin/$applet")" in /*) ;; *) exit 1;; esac
done
test "$(sha256sum "$work/media.img")" = "$before"
PACHA_LIVE_ROOT="$work/root" busybox ash \
  "$repo_root/pack/installer-bootfs/pacha-live-import" "$work/media.img" "$offset"

mkdir -p "$work/verified-root/etc/pacha"
printf 'ram-tmpfs-v1\n' >"$work/verified-root/etc/pacha/boot-profile"
PACHA_LIVE_ROOT="$work/verified-root" busybox ash \
  "$repo_root/pack/installer-bootfs/pacha-live-import" "$work/media.img" "$offset" --verify
test -f "$work/verified-root/run/pacha/live-desktop.ready"
test -f "$work/verified-root/run/pacha/live-desktop.sha256"
rg -q 'usr/libexec/Xorg: OK' "$work/verified-root/run/pacha/live-desktop.log"

mkdir -p "$work/bad-root/etc/pacha" "$work/installed-root/etc/pacha"
printf 'ram-tmpfs-v1\n' >"$work/bad-root/etc/pacha/boot-profile"
printf 'installed\n' >"$work/installed-root/etc/pacha/boot-profile"
printf '%064d  DESKTOP.TGZ\n' 0 >"$work/BAD.SHA"
mcopy -o -i "$image" "$work/BAD.SHA" ::/DESKTOP.SHA
if PACHA_LIVE_ROOT="$work/bad-root" busybox ash \
    "$repo_root/pack/installer-bootfs/pacha-live-import" "$work/media.img" "$offset" --verify; then
    echo 'bad digest was accepted' >&2; exit 1
fi
test ! -e "$work/bad-root/usr/libexec/Xorg"
test ! -e "$work/bad-root/run/pacha/live-desktop.ready"
# A local test boot must not perform archive or per-file digest scans. Host
# image verification remains separate; an explicitly requested scan rejects
# the same bad digest above before writing any payload files.
PACHA_LIVE_ROOT="$work/bad-root" busybox ash \
    "$repo_root/pack/installer-bootfs/pacha-live-import" "$work/media.img" "$offset"
test -f "$work/bad-root/run/pacha/live-desktop.ready"
if PACHA_LIVE_ROOT="$work/installed-root" busybox ash \
    "$repo_root/pack/installer-bootfs/pacha-live-import" "$work/media.img" "$offset"; then
    echo 'installed root was accepted' >&2; exit 1
fi
cc -std=c11 -Wall -Wextra -Werror "$repo_root/tests/installer_runner_policy_unit.c" \
  -o "$work/runner-policy"
"$work/runner-policy"
echo 'PASS: media read-only, archive hash, RAM extraction, inventory, retry and installed-root rejection'
