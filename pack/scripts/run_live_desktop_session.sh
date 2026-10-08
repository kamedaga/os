#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

export HOME=/root USER=root LOGNAME=root SHELL=/bin/ash
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LD_LIBRARY_PATH=/usr/lib:/lib
export LIBGL_DRIVERS_PATH=/usr/lib/dri GBM_BACKENDS_PATH=/usr/lib/gbm
export XDG_RUNTIME_DIR=/run/user/0 DISPLAY=:0
unset LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE
busybox mkdir -p "$XDG_RUNTIME_DIR" /var/log /var/cache/fontconfig
busybox chmod 0700 "$XDG_RUNTIME_DIR"

xinit_pid=0
ready=0
filesystem_usage() {
    # Write to the already-open session output: creating a new diagnostic
    # file would fail at exactly the ENOSPC condition we need to explain.
    echo "--- RAM filesystem usage: $1 ---"
    busybox df -k / 2>&1 || :
    busybox df -i / 2>&1 || :
}
cleanup() {
    result=$?
    trap - EXIT INT TERM
    if [ "$xinit_pid" -gt 0 ]; then
        # xinit's signal handler stops its X server and session groups;
        # killing only startx's shell could strand clients holding DRM fds.
        busybox kill -TERM "$xinit_pid" 2>/dev/null || :
        count=0
        while busybox kill -0 "$xinit_pid" 2>/dev/null && [ "$count" -lt 3 ]; do
            busybox sleep 1
            count=$((count + 1))
        done
        busybox kill -KILL "$xinit_pid" 2>/dev/null || :
        wait "$xinit_pid" 2>/dev/null || :
    fi
    filesystem_usage after-session
    busybox rm -f /run/pacha/desktop-xinit.pid
    echo '--- Xorg log ---'
    busybox cat /run/pacha/Xorg.0.log 2>/dev/null || :
    echo '--- X11 outputs ---'
    busybox cat /run/pacha/desktop-xrandr.log 2>/dev/null || :
    # Keep the terminal status after verbose Xorg output so bounded log readers
    # cannot miss a logout or startup failure at the start of the same file.
    if [ "$ready" -eq 1 ]; then
        echo "LIVE_DESKTOP_SESSION_EXIT status=$result"
    else
        echo "LIVE_DESKTOP_FAIL stage=startup status=$result"
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# The host may disconnect after launch. That is not a request to end an
# interactive desktop; explicit termination still uses xinit's group cleanup.
trap '' HUP

filesystem_usage before-session

# Use upstream xinit/Xorg/Xfce unchanged. TCP is disabled; no authentication
# bypass or machine-specific mode is installed in the normal desktop launcher.
/usr/bin/xinit /root/.xinitrc -- /usr/libexec/Xorg :0 -nolisten tcp \
    -logfile /run/pacha/Xorg.0.log &
xinit_pid=$!
busybox printf '%s\n' "$xinit_pid" > /run/pacha/desktop-xinit.pid
while [ "$ready" -eq 0 ]; do
    if ! busybox kill -0 "$xinit_pid" 2>/dev/null; then
        result=0
        wait "$xinit_pid" || result=$?
        xinit_pid=0
        echo "LIVE_DESKTOP_FAIL stage=xinit-exit status=$result"
        exit 1
    fi
    if [ "$ready" -eq 0 ] && [ -S /tmp/.X11-unix/X0 ]; then
        if busybox timeout -s KILL 3 /usr/bin/xrandr --query \
            > /run/pacha/desktop-xrandr.log 2>&1; then
            echo 'LIVE_DESKTOP_X11_READY display=:0; Xfce visual confirmation still required'
            ready=1
        fi
    fi
    busybox sleep 1
done
# Readiness is not the end of an interactive session. Wait for the user's
# logout rather than killing the server after a fixed observation interval.
echo 'LIVE_DESKTOP_RUNNING until=logout-or-explicit-stop'
result=0
wait "$xinit_pid" || result=$?
xinit_pid=0
exit "$result"
