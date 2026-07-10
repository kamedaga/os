#!/usr/bin/env bash
# Measurement-only T4.5 cold clang startup workload. No performance threshold.

bb=/cmd/busybox
clang=/usr/bin/clang

echo CLANG_COLD_BASELINE_BEGIN
"$bb" sync
echo CLANG_COLD_BASELINE_DONE

start=$("$bb" date +%s)
echo CLANG_COLD_VERSION_BEGIN
"$clang" --version
clang_status=$?
end=$("$bb" date +%s)

echo CLANG_COLD_METRICS_BEGIN
"$bb" sync
echo CLANG_COLD_METRICS_DONE
if [ "$clang_status" -eq 0 ]; then
    echo "CLANG_COLD_MEASURE_OK elapsed_s=$((end - start))"
else
    echo "CLANG_COLD_VERSION_FAIL status=$clang_status"
fi
echo "CLANG_COLD_MEASURE_DONE status=$clang_status elapsed_s=$((end - start))"
