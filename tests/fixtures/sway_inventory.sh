#!/bin/sh

runtime_dir=/tmp
config=/tmp/m51-sway.conf

rm -f "$config"
printf '\n' >"$config"

export XDG_RUNTIME_DIR="$runtime_dir"
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
export SEATD_VTBOUND=0

case "${M51_VARIANT:-default}" in
    allow-software)
        export WLR_RENDERER_ALLOW_SOFTWARE=1
        ;;
    legacy-drm)
        export WLR_RENDERER_ALLOW_SOFTWARE=1
        export WLR_DRM_NO_ATOMIC=1
        ;;
    pixman)
        export WLR_RENDERER=pixman
        ;;
    drm-path)
        export WLR_DRM_DEVICES=/dev/dri/card0
        ;;
    drm-path-legacy)
        export WLR_DRM_DEVICES=/dev/dri/card0
        export WLR_DRM_NO_ATOMIC=1
        export WLR_RENDERER_ALLOW_SOFTWARE=1
        ;;
    drm-only)
        export WLR_BACKENDS=drm
        ;;
    headless)
        export WLR_BACKENDS=headless
        export WLR_HEADLESS_OUTPUTS=1
        export WLR_RENDERER_ALLOW_SOFTWARE=1
        ;;
    headless-pixman)
        export WLR_BACKENDS=headless
        export WLR_HEADLESS_OUTPUTS=1
        export WLR_RENDERER=pixman
        ;;
    headless-client)
        export WLR_BACKENDS=headless
        export WLR_HEADLESS_OUTPUTS=1
        export WLR_RENDERER=pixman
        export M51_CLIENT=/cmd/lpr_wayland_shm_client.elf
        ;;
    headless-libinput-client)
        export WLR_BACKENDS=headless,libinput
        export WLR_HEADLESS_OUTPUTS=1
        export WLR_RENDERER=pixman
        export M51_CLIENT=/cmd/lpr_wayland_shm_client.elf
        ;;
esac

printf 'M51_SEAT_SETUP launcher=%s\n' "${M51_LAUNCH_MODE:-compiled}"

printf 'M51_SWAY_VERSION_BEGIN\n'
/usr/bin/sway --version 2>&1
printf 'M51_SWAY_VERSION_STATUS=%s\n' "$?"

/cmd/lpr_udev_discovery.elf
printf 'M53_UDEV_PROBE_STATUS=%s\n' "$?"

printf 'M51_SWAY_START variant=%s\n' "${M51_VARIANT:-default}"
if [ "${M51_LAUNCH_MODE:-compiled}" = seatd-launch ]; then
    rm -f /run/seatd.sock
    /usr/bin/seatd-launch -- /usr/bin/sway -d -c "$config" 2>&1
else
    /cmd/lpr_sway_launcher.elf /usr/bin/sway -d -c "$config" 2>&1
fi
status=$?
printf 'M51_SWAY_STATUS=%s\n' "$status"
rm -f "$config"
printf 'M51_SWAY_INVENTORY_DONE\n'
exit 0
