#!/bin/sh
# Run from a Linux shell on PachaOS; keep ordinary HotSpot defaults for nulls.
set -u
java=${JAVA25_JAVA:-/opt/java25/bin/java}
classes=${JAVA25_CLASSES:-/opt/java25-tests}
mode=${1:-int}
test_case=${2:-all}
case "$mode" in
    int) set -- -Xint ;;
    jit) set -- -Xbatch '-XX:CompileCommand=dontinline,Java25Probe::*' -XX:+PrintCompilation ;;
    *) echo "unknown Java 25 probe mode: $mode" >&2; exit 2 ;;
esac
printf 'JAVA25_MODE=%s case=%s\n' "$mode" "$test_case"
"$java" -Xms32m -Xmx128m -XX:-CreateCoredumpOnCrash '-XX:ErrorFile=/tmp/java25-hs_err_pid%p.log' \
    "$@" -cp "$classes" Java25Probe "$test_case"
status=$?
printf 'JAVA25_EXIT mode=%s case=%s status=%s\n' "$mode" "$test_case" "$status"
exit "$status"
