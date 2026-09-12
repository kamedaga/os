#!/bin/sh
set -eu
bus_name=
case "$1" in
    --system-bus) bus_name=org.freedesktop.DBus; shift ;;
    --bus-name) bus_name=$2; shift 2 ;;
esac
if [ -n "$bus_name" ]; then
    attempt=0
    while [ "$(/usr/bin/gdbus call --system --timeout 1 --dest org.freedesktop.DBus \
        --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner "$bus_name" 2>/dev/null || true)" != '(true,)' ]; do
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 100 ]; then
            echo "service-runtime: dependency $bus_name did not become ready" >&2
            exit 1
        fi
        /bin/busybox sleep 0.1
    done
fi
# Run under the account and capability grants already assigned by the manager.
# /run is a fresh tmpfs on each boot; packaged directories cannot populate it.
while [ "$#" -gt 0 ] && [ "$1" != -- ]; do
    /bin/busybox mkdir -p "$1"
    shift
done
test "$#" -gt 1
shift
exec "$@"
