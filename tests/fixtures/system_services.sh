#!/bin/sh
set -eu
bus_call() {
    /usr/bin/gdbus call --system --timeout 2 --dest org.freedesktop.DBus \
        --object-path /org/freedesktop/DBus --method "org.freedesktop.DBus.$1" "$2"
}
check_service() {
    name=$1
    uid=$2
    attempt=0
    while [ "$(bus_call NameHasOwner "$name")" != '(true,)' ]; do
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 100 ]; then
            echo "SYSTEM_SERVICES=FAIL missing=$name" >&2
            exit 1
        fi
        /bin/busybox sleep 0.1
    done
    actual=$(bus_call GetConnectionUnixUser "$name")
    test "$actual" = "(uint32 $uid,)"
    echo "SYSTEM_SERVICE=OK name=$name uid=$uid"
}
/cmd/lpr_real_service.elf system
check_service org.freedesktop.DBus 81
check_service org.freedesktop.PolicyKit1 102
check_service org.freedesktop.UPower 0
/usr/bin/gdbus call --system --timeout 2 --dest org.freedesktop.UPower \
    --object-path /org/freedesktop/UPower --method org.freedesktop.UPower.EnumerateDevices
/usr/bin/gdbus call --system --timeout 2 --dest org.freedesktop.PolicyKit1 \
    --object-path /org/freedesktop/PolicyKit1/Authority --method org.freedesktop.DBus.Peer.Ping
/usr/bin/gdbus call --system --timeout 2 --dest org.freedesktop.PolicyKit1 \
    --object-path /org/freedesktop/PolicyKit1/Authority \
    --method org.freedesktop.PolicyKit1.Authority.EnumerateActions '' >/dev/null
echo SYSTEM_SERVICES=OK
