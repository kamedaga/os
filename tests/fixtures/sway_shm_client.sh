#!/bin/sh

config=/tmp/m56-sway.conf
rm -f "$config"
printf 'swaybg_command -\nxwayland disable\nfor_window [app_id="m56-shm"] floating enable, border none, resize set 256 192, move position 384 288\n' >"$config"

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
export M51_CLIENT=/cmd/lpr_wayland_shm_client.elf

printf 'M56_SWAY_START backend=drm client=wl_shm\n'
/cmd/lpr_sway_launcher.elf /usr/bin/sway -d -c "$config" 2>&1
status=$?
rm -f "$config"
/bin/sync
printf 'M56_SHM_SYNC_DONE\n'
printf 'M56_SWAY_STATUS=%s\n' "$status"
exit "$status"
