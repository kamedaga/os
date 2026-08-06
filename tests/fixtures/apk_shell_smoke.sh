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

apk --progress=no add nano
command -v nano
printf 'APK_SHELL_ADD_NANO=OK\n'

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

sync
printf 'APK_SHELL_SYNC=OK\n'
