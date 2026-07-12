#!/bin/sh

config=/tmp/m52-repeat.conf
printf '\n' >"$config"
export XDG_RUNTIME_DIR=/tmp
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
export SEATD_VTBOUND=0

i=1
failed=0
while [ "$i" -le 20 ]; do
    /cmd/lpr_sway_launcher.elf /usr/bin/sway -c "$config" >/dev/null 2>&1
    if [ -e /run/seatd.sock ]; then
        printf 'M52_STALE_SOCKET iteration=%s\n' "$i"
        failed=1
        break
    fi
    printf 'M52_SOCKET_ITERATION=%s stale=0\n' "$i"
    i=$((i + 1))
done
rm -f "$config" /run/seatd.sock
printf 'M52_SOCKET_REPEAT_STATUS=%s completed=%s\n' "$failed" "$((i - 1))"
exit "$failed"
