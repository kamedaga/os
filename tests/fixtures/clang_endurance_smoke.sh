#!/usr/bin/env bash
# PachaOS T4.5 clang staged recovery + endurance workload.

bb=/cmd/busybox
clang=/usr/bin/clang
hello_src=/cmd/clang_hello.c
hello_bin=/tmp/clang_hello

echo CLANG_VERSION_BEGIN
if ! "$clang" --version; then
    echo CLANG_VERSION_FAIL
    exit 1
fi
echo CLANG_VERSION_OK

echo CLANG_OBJECT_STRESS_BEGIN
"$bb" find /usr/include -type f | "$bb" head -n 140 >/tmp/clang_object_paths
object_count=$("$bb" wc -l </tmp/clang_object_paths)
if [ "$object_count" -lt 120 ]; then
    echo "CLANG_OBJECT_STRESS_FAIL count=$object_count"
    exit 1
fi
while IFS= read -r path; do
    if ! "$bb" stat "$path" >/dev/null; then
        echo "CLANG_OBJECT_STRESS_FAIL path=$path"
        exit 1
    fi
done </tmp/clang_object_paths
echo "CLANG_OBJECT_STRESS_OK count=$object_count"

echo CLANG_COMPILE_ONLY_BEGIN
if ! "$clang" -c /cmd/chibicc_workload.c -o /tmp/w.o; then
    echo CLANG_COMPILE_ONLY_FAIL
    exit 1
fi
echo CLANG_COMPILE_ONLY_OK

echo CLANG_LINK_RUN_BEGIN
if ! "$clang" "$hello_src" -o "$hello_bin"; then
    echo CLANG_LINK_FAIL
    exit 1
fi
hello_output=$("$hello_bin")
if [ "$hello_output" != "T4.5 hello" ]; then
    echo "CLANG_RUN_FAIL output=[$hello_output]"
    exit 1
fi
echo CLANG_LINK_RUN_OK
"$bb" rm -f "$hello_bin"

echo CLANG_KOBOX_BASELINE
"$bb" sync
echo CLANG_KOBOX_BASELINE_DONE

total_start=$("$bb" date +%s)
echo CLANG_STATE_BEFORE
"$bb" true
echo CLANG_STATE_BEFORE_DONE
i=1
while [ "$i" -le 10 ]; do
    iter_start=$("$bb" date +%s)
    if ! "$clang" "$hello_src" -o "$hello_bin"; then
        echo "CLANG_ENDURANCE_FAIL iteration=$i stage=compile"
        exit 1
    fi
    hello_output=$("$hello_bin")
    if [ "$hello_output" != "T4.5 hello" ]; then
        echo "CLANG_ENDURANCE_FAIL iteration=$i stage=run output=[$hello_output]"
        exit 1
    fi
    "$bb" rm -f "$hello_bin"
    "$bb" sync
    iter_end=$("$bb" date +%s)
    echo "CLANG_ENDURANCE_ITER_OK iteration=$i elapsed_s=$((iter_end - iter_start))"
    i=$((i + 1))
done
total_end=$("$bb" date +%s)

echo CLANG_KOBOX_AFTER
"$bb" sync
echo CLANG_KOBOX_AFTER_DONE
echo CLANG_STATE_AFTER
"$bb" true
echo CLANG_STATE_AFTER_DONE
echo "CLANG_ENDURANCE_DONE iterations=10 elapsed_s=$((total_end - total_start))"
