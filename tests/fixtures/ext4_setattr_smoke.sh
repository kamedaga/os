#!/bin/bash
set -euo pipefail

path=/var/tmp/ext4-setattr-smoke
mkdir -p /var/tmp
rm -f "$path"
printf 'abcdef' > "$path"
chmod 600 "$path"
printf 'EXT4_SETATTR_CHMOD=OK\n'
truncate -s 3 "$path"
test "$(wc -c < "$path")" = 3
printf 'EXT4_SETATTR_TRUNCATE=OK\n'
touch -t 202001020304 "$path"
printf 'EXT4_SETATTR_UTIMENS=OK\n'
rm "$path"
sync
printf 'EXT4_SETATTR_DONE\n'
