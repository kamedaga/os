#!/bin/bash
set -euo pipefail

# Exercise bash/LPR foreground child reaping and later background jobs without
# involving ext4.  Reuse a bounded set of tmpfs names so filed's tmpfs inode
# limit does not become the result of the probe.
root=/tmp/lpr-bash-file-churn
rm -rf -- "$root"
mkdir -p "$root"

for round in $(seq 0 15); do
    batch="$root/b$round"
    mkdir "$batch"
    for file in $(seq 0 31); do
        path="$batch/f$file"
        printf 'round=%d file=%d\n' "$round" "$file" > "$path"
        chmod "$((600 + (file % 8) * 11))" "$path"
        mv "$path" "$path.new"
        printf 'replacement\n' >> "$path.new"
        truncate -s "$((file * 137))" "$path.new"
        mv "$path.new" "$path"
        rm "$path"
    done
    rmdir "$batch"
done
printf 'LPR_BASH_FILE_CHURN_FOREGROUND=OK operations=3072\n'

pids=
for worker in 0 1 2 3; do
    (
        for file in $(seq 0 63); do
            path="$root/w${worker}-$((file % 4))"
            printf 'worker=%d file=%d\n' "$worker" "$file" > "$path"
            if (( file % 3 == 0 )); then
                mv "$path" "$path.new"
                mv "$path.new" "$path"
            elif (( file % 3 == 1 )); then
                rm "$path"
            else
                truncate -s "$((file * 97))" "$path"
            fi
        done
        printf 'LPR_BASH_FILE_CHURN_WORKER=%d DONE\n' "$worker"
    ) &
    pids="$pids $!"
done

status=0
for pid in $pids; do
    if ! wait "$pid"; then
        status=1
    fi
done
test "$status" = 0
rm -rf -- "$root"
printf 'LPR_BASH_FILE_CHURN_DONE\n'
