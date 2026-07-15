#!/bin/sh

config=/tmp/p6-wayland-info-sway.conf
rm -f "$config"
printf 'swaybg_command -\nxwayland disable\n' >"$config"

export XDG_RUNTIME_DIR=/tmp
export WAYLAND_DISPLAY=wayland-1
export PATH=/usr/bin:/bin:/cmd
export WLR_BACKENDS=drm
export WLR_RENDERER=gles2
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
export SEATD_VTBOUND=0
export MESA_SHADER_CACHE_DISABLE=true
export M55_FIRST_FRAME=1

printf 'P6_WAYLAND_INFO_SWAY_START backend=drm client=/usr/bin/wayland-info\n'
printf 'P6_WAYLAND_INFO_EXEC path=/usr/bin/wayland-info\n'
/cmd/lpr_sway_launcher.elf \
    --client /usr/bin/wayland-info \
    -- /usr/bin/sway -d -c "$config" 2>&1
launcher_status=$?
rm -f "$config"
/bin/sync
printf 'P6_WAYLAND_INFO_LAUNCHER_STATUS=%s\n' "$launcher_status"
printf 'P6_WAYLAND_INFO_PROBE_DONE\n'
exit 0
