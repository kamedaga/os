#!/bin/sh

iterations=${M58_ITERATIONS:-10}
case "$iterations" in
    ''|*[!0-9]*) echo "M58_BAD_ITERATIONS value=$iterations"; exit 2 ;;
esac
if [ "$iterations" -lt 1 ] || [ "$iterations" -gt 20 ]; then
    echo "M58_BAD_ITERATIONS value=$iterations"
    exit 2
fi

config=/tmp/m58-sway.conf
rm -f "$config"
printf 'swaybg_command -\nxwayland disable\ninput type:pointer accel_profile flat\nseat seat0 cursor set 400 300\nfor_window [app_id="m56-shm"] floating enable, border none, resize set 256 192, move position 384 288\n' >"$config"

export XDG_RUNTIME_DIR=/tmp
export WAYLAND_DISPLAY=wayland-1
export PATH=/usr/bin:/bin:/cmd
export WLR_RENDERER=gles2
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
export SEATD_VTBOUND=0
export MESA_SHADER_CACHE_DISABLE=true
export M55_FIRST_FRAME=1
export M51_CLIENT=/cmd/lpr_wayland_shm_client.elf

i=1
while [ "$i" -le "$iterations" ]; do
    # Keep four deterministic forced-exit samples in the configurable run.
    # Each one also leaves five ownerless drmd handles (Phase 6); more than
    # five forced exits exhausts DRMD_HANDLE_MAX=32 before the next boot.
    case "$i" in
        2|7) mode=term ;;
        4|9) mode=kill ;;
        *) mode=normal ;;
    esac
    export M58_ITERATION="$i"
    export M56_LIFECYCLE_MODE="$mode"
    if [ "$i" -eq 1 ]; then
        export WLR_BACKENDS=drm,libinput
        export M57_INPUT=1
        export M57_WLROOTS_KEYMAP_PRELOAD=/cmd/libm57-wlroots-keymap-compat.so
    else
        export WLR_BACKENDS=drm
        unset M57_INPUT M57_WLROOTS_KEYMAP_PRELOAD
    fi

    printf 'M58_ITERATION_BEGIN iteration=%s mode=%s input=%s\n' \
        "$i" "$mode" "$([ "$i" -eq 1 ] && echo 1 || echo 0)"
    /cmd/lpr_sway_launcher.elf /usr/bin/sway -d -c "$config" 2>&1
    status=$?
    if [ "$status" -ne 0 ]; then
        printf 'M58_LAUNCHER_FAIL iteration=%s mode=%s status=%s\n' "$i" "$mode" "$status"
        rm -f "$config"
        exit 1
    fi
    if [ -e /run/seatd.sock ] || [ -e /tmp/wayland-1 ] || [ -e /tmp/wayland-1.lock ]; then
        printf 'M58_STALE_SOCKET iteration=%s mode=%s\n' "$i" "$mode"
        rm -f "$config"
        exit 1
    fi
    printf 'M58_FILED_CHECKPOINT_BEGIN iteration=%s\n' "$i"
    /bin/sync
    printf 'M58_ITERATION_PASS iteration=%s mode=%s orphan=0 stale=0\n' "$i" "$mode"
    i=$((i + 1))
done

rm -f "$config"
printf 'M58_ENDURANCE_PASS completed=%s\n' "$iterations"
