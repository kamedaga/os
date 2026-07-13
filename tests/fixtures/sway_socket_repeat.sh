#!/bin/sh

config=/run/m52-repeat.conf
printf 'swaybg_command -\nxwayland disable\n' >"$config"
export XDG_RUNTIME_DIR=/run
export WAYLAND_DISPLAY=wayland-1
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
export SEATD_VTBOUND=0
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=0
export WLR_RENDERER=pixman
export MESA_SHADER_CACHE_DISABLE=true
export M56_LIFECYCLE_MODE=${M56_MODE:-kill}

iterations=${M52_ITERATIONS:-20}
i=1
failed=0
while [ "$i" -le "$iterations" ]; do
    /cmd/lpr_sway_launcher.elf /usr/bin/sway -c "$config"
    launcher_status=$?
    if [ "$launcher_status" -ne 0 ]; then
        printf 'M56_LIFECYCLE_LAUNCHER_FAIL iteration=%s mode=%s status=%s\n' \
            "$i" "$M56_LIFECYCLE_MODE" "$launcher_status"
        failed=1
        break
    fi
    if [ -e /run/seatd.sock ] ||
       [ -e "$XDG_RUNTIME_DIR/wayland-1" ] ||
       [ -e "$XDG_RUNTIME_DIR/wayland-1.lock" ]; then
        printf 'M52_STALE_SOCKET iteration=%s\n' "$i"
        failed=1
        break
    fi
    printf 'M52_SOCKET_ITERATION=%s stale=0 mode=%s\n' "$i" "$M56_LIFECYCLE_MODE"
    i=$((i + 1))
done
rm -f "$config" /run/seatd.sock
/bin/sync
printf 'M52_SOCKET_REPEAT_STATUS=%s completed=%s mode=%s\n' \
    "$failed" "$((i - 1))" "$M56_LIFECYCLE_MODE"
exit "$failed"
