#!/bin/bash
set -euo pipefail
iterations=${1:-5}
printf 'NVME_STRESS_BEGIN iterations=%s\n' "$iterations"
memory_checkpoint() {
    # The values come from the existing SYSTEM_INFO-backed proc adapter;
    # compare command boundaries instead of masking exhaustion with more RAM.
    printf 'NVME_MEMORY_STAGE=%s\n' "$1"
    cat /proc/meminfo
}
memory_checkpoint begin
command -v apk
apk --version
printf 'APK_VERSION_OK\n'
cat /etc/apk/repositories
apk --no-progress update
printf 'APK_UPDATE_OK\n'
memory_checkpoint apk-update
if apk info -e nano >/dev/null 2>&1; then apk --no-progress del nano; fi
# Real transactions exercise signed archives, database publication,
# extraction and package deletion; synthetic file churn is not a substitute.
for ((iteration=1; iteration<=iterations; iteration++)); do
    apk --no-progress add nano
    apk info -e nano
    nano --version
    test -x /usr/bin/nano
    printf 'APK_ADD_OK iteration=%s\n' "$iteration"
    apk --no-progress del nano
    if apk info -e nano >/dev/null 2>&1; then exit 1; fi
    test ! -e /usr/bin/nano
    printf 'APK_DEL_OK iteration=%s\n' "$iteration"
    memory_checkpoint "apk-$iteration"
done
clang --version
printf 'CLANG_VERSION_OK\n'
# Put outputs on NVMe ext4, not /tmp's tmpfs. Preprocessing also opens the
# installed system headers, while the linker reads the real CRT and libraries.
work=/root/filed-nvme-clang
mkdir "$work"
clang -Wall -Wextra -Werror -c /cmd/clang_hello.c -o "$work/hello.o"
test -s "$work/hello.o"
printf 'CLANG_COMPILE_ONLY_OK\n'
memory_checkpoint clang-compile-only
clang "$work/hello.o" -o "$work/hello"
test "$("$work/hello")" = 'T4.5 hello'
printf 'CLANG_LINK_RUN_OK\n'
memory_checkpoint clang-link-run
for ((iteration=1; iteration<=iterations; iteration++)); do
    clang -Wall -Wextra -Werror /cmd/clang_hello.c -o "$work/hello.new"
    mv "$work/hello.new" "$work/hello"
    test "$("$work/hello")" = 'T4.5 hello'
    rm "$work/hello"
    sync
    printf 'CLANG_ITER_OK iteration=%s\n' "$iteration"
    memory_checkpoint "clang-$iteration"
done
rm "$work/hello.o"
rmdir "$work"
sync
printf 'NVME_STRESS_SYNC_OK\n'
