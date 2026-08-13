#!/bin/bash
set -euo pipefail

root=/var/tmp/ext4-parallel-probe
rm -rf "$root"
mkdir -p "$root"
pids=
for worker in 0 1 2 3; do
    (
        for file in $(seq 0 15); do
            printf 'EXT4_PARALLEL worker=%d file=%d begin\n' "$worker" "$file"
            path="$root/w${worker}-$file"
            printf 'worker=%d file=%d\n' "$worker" "$file" > "$path"
            if (( file % 3 == 0 )); then
                mv "$path" "$path.renamed"
            elif (( file % 3 == 1 )); then
                rm "$path"
            else
                truncate -s "$((file * 97))" "$path"
            fi
            printf 'EXT4_PARALLEL worker=%d file=%d done\n' "$worker" "$file"
        done
    ) &
    pids="$pids $!"
done
for pid in $pids; do
    wait "$pid"
done
rm -rf "$root"
sync
printf 'EXT4_PARALLEL_DONE\n'
