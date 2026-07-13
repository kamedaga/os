#!/bin/sh

config=/tmp/m55-sway.conf
rm -f "$config"
printf 'swaybg_command -\nxwayland disable\n' >"$config"

export XDG_RUNTIME_DIR=/tmp
export WLR_BACKENDS=drm
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
export SEATD_VTBOUND=0
export M55_FIRST_FRAME=1

printf 'M55_SWAY_START backend=drm card=/dev/dri/card0\n'
/cmd/lpr_sway_launcher.elf /usr/bin/sway -d -c "$config" 2>&1
status=$?
# Persist the Mesa shader cache so later runs start warm.
/bin/sync
printf 'M55_SWAY_STATUS=%s\n' "$status"
exit "$status"
