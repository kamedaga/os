#!/bin/sh

result=/tmp/pty_teardown_result
input=/tmp/pty_teardown_input
rm -f "$result" "$input"
printf '%s\n' pty-teardown >"$input"

failures=0
if /cmd/busybox timeout -s KILL 6 /cmd/pty_teardown_driver.elf; then
    driver_status=0
else
    driver_status=$?
fi

i=1
while [ "$i" -le 3 ]; do
    if [ "$driver_status" -eq 0 ]; then
        printf 'PTY_TEARDOWN_GREP_%s=OK\n' "$i" >>"$result"
    else
        printf 'PTY_TEARDOWN_GREP_%s=FAIL status=%s\n' "$i" "$driver_status" >>"$result"
        failures=$((failures + 1))
    fi
    if [ "$driver_status" -eq 0 ]; then
        printf 'PTY_TEARDOWN_SLEEP_%s=OK\n' "$i" >>"$result"
    else
        printf 'PTY_TEARDOWN_SLEEP_%s=FAIL status=%s\n' "$i" "$driver_status" >>"$result"
        failures=$((failures + 1))
    fi
    i=$((i + 1))
done

printf 'PTY_TEARDOWN_DONE failures=%s\n' "$failures" >>"$result"
printf 'PTY_TEARDOWN_OUTPUT\n' || true
while IFS= read -r line; do
    printf '%s\n' "$line"
done <"$result"

rm -f "$result" "$input"
[ "$failures" -eq 0 ]
