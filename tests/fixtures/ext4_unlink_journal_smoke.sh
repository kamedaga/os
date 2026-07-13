#!/bin/sh

iterations="${1:-40}"
workdir=/var/lib
target="$workdir/ext4-unlink-journal.node"
failures=0

case "$iterations" in
    ''|*[!0-9]*|0)
        echo "EXT4_UNLINK_JOURNAL_BAD_ITERATIONS value=$iterations"
        exit 2
        ;;
esac

/cmd/busybox rm -f "$target" 2>/dev/null
if [ ! -d "$workdir" ]; then
    echo "EXT4_UNLINK_JOURNAL_SETUP=FAIL"
    exit 1
fi

echo "EXT4_UNLINK_JOURNAL_START iterations=$iterations"
i=1
while [ "$i" -le "$iterations" ]; do
    if ! /cmd/busybox sh -c 'printf "%s\n" "$1" >"$2"' sh "$i" "$target"; then
        echo "EXT4_UNLINK_JOURNAL_CREATE=FAIL iteration=$i phase=initial"
        failures=$((failures + 1))
        break
    fi
    if [ ! -f "$target" ]; then
        echo "EXT4_UNLINK_JOURNAL_LOOKUP=FAIL iteration=$i phase=created"
        failures=$((failures + 1))
        break
    fi

    /bin/sync
    if ! /cmd/busybox rm "$target"; then
        echo "EXT4_UNLINK_JOURNAL_UNLINK=FAIL iteration=$i"
        failures=$((failures + 1))
        break
    fi
    /bin/sync

    if [ -e "$target" ]; then
        echo "EXT4_UNLINK_JOURNAL_ENOENT=FAIL iteration=$i"
        failures=$((failures + 1))
        break
    fi
    if ! /cmd/busybox sh -c ': >"$1"' sh "$target"; then
        echo "EXT4_UNLINK_JOURNAL_RECREATE=FAIL iteration=$i"
        failures=$((failures + 1))
        break
    fi
    if [ ! -f "$target" ]; then
        echo "EXT4_UNLINK_JOURNAL_LOOKUP=FAIL iteration=$i phase=recreated"
        failures=$((failures + 1))
        break
    fi
    if ! /cmd/busybox rm "$target"; then
        echo "EXT4_UNLINK_JOURNAL_CLEANUP=FAIL iteration=$i"
        failures=$((failures + 1))
        break
    fi
    /bin/sync

    echo "EXT4_UNLINK_JOURNAL_ITERATION=$i status=OK"
    i=$((i + 1))
done

/cmd/busybox rm -f "$target" 2>/dev/null
/bin/sync
completed=$((i - 1))
echo "EXT4_UNLINK_JOURNAL_DONE iterations=$iterations completed=$completed failures=$failures"
if [ "$failures" -ne 0 ] || [ "$completed" -ne "$iterations" ]; then
    exit 1
fi

/cmd/lpr_ext4_unlink_journal_smoke.elf "$iterations" "$workdir/ext4-unlink-journal.sock"
