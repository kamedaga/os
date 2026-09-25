#!/bin/sh
# Run only inside the disposable Alpine comparison VM. The PachaOS disk is
# attached read-only; overlay writes and runtime state live entirely in RAM.
set -eu
mkdir -p /pacha /cow /linux
mount -t ext4 -o ro,noload /dev/nvme0n1p2 /pacha
mount -t tmpfs -o size=512m tmpfs /cow
mkdir /cow/upper /cow/work
modprobe overlay
mount -t overlay overlay -o lowerdir=/pacha,upperdir=/cow/upper,workdir=/cow/work /linux
mount --rbind /dev /linux/dev
mount -t proc proc /linux/proc
mount --rbind /sys /linux/sys
mount -t tmpfs tmpfs /linux/run
mount -t tmpfs tmpfs /linux/tmp
if test "${GTK_FRAME_CLOCK_TRIAL:-0}" = 1; then
    mkdir /linux/tmp/gtk-frameclock
    mount -o ro /dev/vdb1 /linux/tmp/gtk-frameclock
    sha256sum /linux/tmp/gtk-frameclock/libgdk-3.so.0
fi
# /lib/ld-musl is the native PachaOS loader in the shared image. Use the
# existing, unmodified Alpine Linux musl for Linux exec; do not rewrite it.
mount --bind /pacha/lib/libc.musl-x86_64.so.1 /linux/lib/ld-musl-x86_64.so.1
mount --bind /pacha/lib/libc.musl-x86_64.so.1 /linux/lib/libc.so
# Optional Linux-only A/B library. Both the download and bind mount are in
# this VM's RAM; the attached PachaOS disk remains read-only.
if test -n "${MESA_TRIAL_URL:-}"; then
    wget -q -O /cow/mesa-trial.so "$MESA_TRIAL_URL"
    sha256sum /cow/mesa-trial.so | tee /linux/tmp/mesa-trial.sha256
    mount --bind /cow/mesa-trial.so /linux/usr/lib/libgallium-25.1.9.so
fi
modprobe virtio_gpu
if test "${FISHBOWL_UNSYNCED:-0}" = 1; then
    # Only this VM's RAM overlay is changed; installed OSS stays untouched.
    for config in \
        /linux/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml \
        /linux/root/.config/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml; do
        if test -f "$config"; then
            sed -i '/name="vblank_mode"/s/value="[^"]*"/value="off"/' "$config"
        fi
    done
    export vblank_mode=0
fi
mkdir -p /linux/run/dbus /linux/run/user/0
chmod 700 /linux/run/user/0
chroot /linux /usr/bin/dbus-daemon --system --fork
chroot /linux /usr/bin/Xorg :0 -noreset -listen tcp -ac vt1 >/linux/tmp/linux-xorg.log 2>&1 &
for attempt in $(seq 1 100); do
    test ! -S /linux/tmp/.X11-unix/X0 || break
    sleep .1
done
test -S /linux/tmp/.X11-unix/X0
export DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/0
if test "${FISHBOWL_UNSYNCED:-0}" = 1; then
    output=$(chroot /linux /usr/bin/xrandr | awk '/ connected/ {print $1; exit}')
    test -n "$output"
    chroot /linux /usr/bin/xrandr --newmode fishbowl-240 \
        100.8 640 656 752 800 480 490 492 525 -hsync -vsync
    chroot /linux /usr/bin/xrandr --addmode "$output" fishbowl-240
    chroot /linux /usr/bin/xrandr --output "$output" --mode fishbowl-240
    chroot /linux /usr/bin/xrandr --verbose
fi
chroot /linux /usr/bin/dbus-run-session -- /usr/bin/xfce4-session >/linux/tmp/linux-xfce.log 2>&1 &
echo LINUX_XFCE_STARTED
