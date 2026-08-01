#!/bin/sh
# Interactive watch variant of sway_foot_probe.sh: keeps foot mapped on screen
# for WATCH_SECONDS so it can be observed live in the QEMU display window.
# Not an oracle/regression test — purely for eyeballing foot rendering.

watch_seconds="${WATCH_SECONDS:-90}"

config=/tmp/p6-foot-watch-sway.conf
rm -f "$config"
printf 'swaybg_command -\nxwayland disable\ninput type:pointer accel_profile flat\nseat seat0 attach 1575:2:QEMU_Virtio_Mouse\nseat seat0 fallback false\nseat seat0 cursor set 512 384\n' >"$config"

export XDG_RUNTIME_DIR=/tmp
export WAYLAND_DISPLAY=wayland-1
export PATH=/usr/bin:/bin:/cmd
export WLR_BACKENDS=drm,libinput
export WLR_RENDERER=gles2
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
export SEATD_VTBOUND=0
export MESA_SHADER_CACHE_DISABLE=true
export M55_FIRST_FRAME=1
export M51_CLIENT_TIMEOUT_SECONDS="$watch_seconds"

printf 'P6_FOOT_WATCH_START seconds=%s\n' "$watch_seconds"
# The PTY child prints a banner then blocks on read (no forks, no CPU spin) so
# foot stays mapped until the launcher's client timeout SIGKILLs sway.
/cmd/lpr_sway_launcher.elf \
    --client /usr/bin/foot /cmd/busybox sh -c \
    'printf "PACHA_FOOT_OK (watch)\n"; printf "foot on PachaOS / Sway\n"; read _' \
    -- /usr/bin/sway -d -c "$config" &
launcher_pid=$!

# Give sway+foot time to map and draw, then announce a screenshot point so a
# host screendump can catch foot (and the cursor) fully rendered.
tick=0
while [ "$tick" -lt "${WATCH_SHOT_DELAY:-45}" ] && kill -0 "$launcher_pid" 2>/dev/null; do
    sleep 1
    tick=$((tick + 1))
done
printf 'P6_FOOT_WATCH_SHOT tick=%s\n' "$tick"

wait "$launcher_pid"
rm -f "$config"
/bin/sync
printf 'P6_FOOT_WATCH_DONE\n'
exit 0
