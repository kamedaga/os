#!/bin/sh

config=/tmp/p6-foot-sway.conf
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
export M51_CLIENT_TIMEOUT_SECONDS=30

pty_marker=/tmp/p6-foot-pty-command-ok
pty_release=/tmp/p6-foot-pty-release
launcher_log=/tmp/p6-foot-launcher.log
rm -f "$pty_marker" "$pty_release" "$launcher_log"

printf 'P6_FOOT_SWAY_START backend=drm client=/usr/bin/foot\n'
printf 'P6_FOOT_EXEC path=/usr/bin/foot command=/cmd/busybox sh\n'
/cmd/lpr_sway_launcher.elf \
    --client /usr/bin/foot /cmd/busybox sh -c \
    'printf "PACHA_FOOT_OK\n"; : > /tmp/p6-foot-pty-command-ok; while [ ! -e /tmp/p6-foot-pty-release ]; do :; done; exec /cmd/busybox true' \
    -- /usr/bin/sway -d -c "$config" >"$launcher_log" 2>&1 &
launcher_pid=$!

tick=0
while [ "$tick" -lt 600 ] && [ ! -e "$pty_marker" ]; do
    if ! kill -0 "$launcher_pid" 2>/dev/null; then
        break
    fi
    sleep 0.1
    tick=$((tick + 1))
done
if [ -e "$pty_marker" ]; then
    printf 'P6_FOOT_PTY_COMMAND_OK\n'
    # The PTY command stays alive through the initial xdg-toplevel configure.
    sleep 5
    if kill -0 "$launcher_pid" 2>/dev/null; then
        printf 'P6_FOOT_ALIVE_AT_SCREENSHOT=1\n'
    else
        printf 'P6_FOOT_ALIVE_AT_SCREENSHOT=0\n'
    fi
else
    printf 'P6_FOOT_PTY_COMMAND_MISSING\n'
fi
printf 'P6_FOOT_SCREENSHOT_POINT\n'
# Leave the mapped client visible while the host captures the screendump, then
# let the one PTY child exit successfully to exercise foot's SIGCHLD/reap path.
sleep 2
: > "$pty_release"
printf 'P6_FOOT_CHILD_RELEASED\n'

wait "$launcher_pid"
launcher_status=$?
cat "$launcher_log"
client_exit_ok=0
while IFS= read -r line; do
    case "$line" in
        *M51_LAUNCHER_CLIENT_EXIT=0*) client_exit_ok=1 ;;
    esac
done <"$launcher_log"
if [ "$client_exit_ok" -eq 1 ]; then
    printf 'P6_FOOT_CHILD_EXITED=1\n'
    printf 'P6_FOOT_EXIT=0\n'
else
    printf 'P6_FOOT_CHILD_EXITED=0\n'
    printf 'P6_FOOT_EXIT=FAIL\n'
fi
rm -f "$config"
/bin/sync
printf 'P6_FOOT_LAUNCHER_STATUS=%s\n' "$launcher_status"
printf 'P6_FOOT_PROBE_DONE\n'
rm -f "$pty_marker" "$pty_release" "$launcher_log"
exit 0
