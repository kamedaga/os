#!/bin/sh

runtime_dir=/tmp
result=/tmp/m51-sway-result
status_file=/tmp/m51-sway-status
config=/tmp/m51-sway.conf

rm -f "$result" "$status_file" "$config"
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

printf 'M51_SEAT_SETUP launcher=compiled\n'

printf 'M51_SWAY_VERSION_BEGIN\n'
/usr/bin/sway --version 2>&1
printf 'M51_SWAY_VERSION_STATUS=%s\n' "$?"

printf 'M51_SWAY_START variant=%s\n' "${M51_VARIANT:-default}"
(/cmd/lpr_sway_launcher.elf /usr/bin/sway -d -c "$config" 2>&1; printf '%s\n' "$?" >"$status_file") | busybox tee -a "$result"
status=$(busybox cat "$status_file")
printf 'M51_SWAY_STATUS=%s\n' "$status"
printf 'M51_SEATD_LOG_BEGIN\n'
busybox cat "$result"
printf 'M51_SEATD_LOG_END\n'
rm -f "$result" "$status_file" "$config"
printf 'M51_SWAY_INVENTORY_DONE\n'
exit 0
