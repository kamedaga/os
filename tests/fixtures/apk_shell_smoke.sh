#!/bin/bash
set -euo pipefail

apk_path=$(command -v apk)
if [[ $apk_path != /bin/apk && $apk_path != /usr/bin/apk ]]; then
    printf 'APK_SHELL_FAIL stage=path value=%s\n' "${apk_path:-missing}"
    exit 1
fi
printf 'APK_SHELL_PATH=%s\n' "$apk_path"

apk --version
printf 'APK_SHELL_VERSION=OK\n'

apk --repositories-file /dev/null \
    --no-network \
    --repository /var/cache/apk/offline/main \
    --repository /var/cache/apk/offline/community \
    --progress=no update
printf 'APK_SHELL_OFFLINE_INDEX=OK\n'

apk --progress=no update
printf 'APK_SHELL_UPDATE=OK\n'

if apk info -e nano >/dev/null 2>&1; then
    apk --progress=no del nano
fi

# Exercise apk's real package database, archive extraction, rename, unlink,
# directory cleanup, and fsync paths repeatedly.  Three cycles are long enough
# to catch the previously recurring ext4 lifecycle fault without turning this
# local acceptance test into an endurance run.
apk_iterations=${APK_SHELL_ITERATIONS:-3}
for iteration in $(seq 1 "$apk_iterations"); do
    apk --progress=no add nano
    command -v nano
    printf 'APK_SHELL_NANO_ADD iteration=%s\n' "$iteration"

    apk --progress=no del nano
    if apk info -e nano >/dev/null 2>&1; then
        printf 'APK_SHELL_FAIL stage=del-nano-still-installed iteration=%s\n' "$iteration"
        exit 1
    fi
    hash -r
    if command -v nano >/dev/null 2>&1; then
        printf 'APK_SHELL_FAIL stage=del-nano-command-present iteration=%s\n' "$iteration"
        exit 1
    fi
    printf 'APK_SHELL_NANO_DEL iteration=%s\n' "$iteration"
done
printf 'APK_SHELL_NANO_CYCLES=OK iterations=%s\n' "$apk_iterations"

apk --progress=no add grep
grep --version | head -n 1
printf 'APK_SHELL_ADD_GREP=OK\n'

apk --progress=no add wget
wget --version | head -n 1
test "$(stat -c '%a' /usr/bin/wget)" = 755
printf 'APK_SHELL_ADD_WGET=OK\n'

apk --progress=no add fastfetch
fastfetch --version
test "$(stat -c '%a' /usr/bin/fastfetch)" = 755
printf 'APK_SHELL_ADD_FASTFETCH=OK\n'

if apk info -e nano >/dev/null 2>&1; then
    printf 'APK_SHELL_FAIL stage=final-nano-installed\n'
    exit 1
fi
printf 'APK_SHELL_FINAL_NANO_ABSENT=OK\n'

sync
printf 'APK_SHELL_SYNC=OK\n'
