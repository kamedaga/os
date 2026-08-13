#!/bin/sh
set -eu

# /tmp is filed's bounded tmpfs.  Keep this workload on the ext4 rootfs so an
# ENOSPC result reflects the storage stack under test rather than tmpfs policy.
root=${EXT4_APK_REPRO_ROOT:-/var/tmp/ext4-apk-reproducer}
sync_sha256=5f52c600ccfc5c066964df5c53b3e139028e20c440d62143c3d66a0e63031b6c

# Keep the diagnostic reader itself resident before applying page-cache
# pressure.  The immutable rootfs binary below is the canary for accidental
# cross-inode folio reuse: the workload never opens it for write.
/usr/bin/sha256sum --version >/dev/null
test "$(/usr/bin/sha256sum /bin/sync | while read -r sum _; do printf '%s' "$sum"; done)" = "$sync_sha256"
printf 'EXT4_APK_REPRO_IMMUTABLE_BEFORE=OK sha256=%s\n' "$sync_sha256"

rm -rf -- "$root"
mkdir -p "$root/tree" "$root/parallel"

# APK extraction shape: many small files spread over directory blocks, followed
# by replacement renames, truncation, unlink, fsync and concurrent creators.
for dir in $(seq 0 15); do
    mkdir "$root/tree/d$dir"
    for file in $(seq 0 31); do
        path="$root/tree/d$dir/f$file"
        printf 'package=%02d file=%02d\npayload=%08x\n' \
            "$dir" "$file" "$((dir * 65536 + file))" > "$path"
        chmod "$((600 + (file % 8) * 11))" "$path"
    done
done
printf 'EXT4_APK_REPRO_CREATE=OK files=512\n'

for dir in $(seq 0 15); do
    printf 'EXT4_APK_REPRO_RENAME_DIR=%d BEGIN\n' "$dir"
    for file in $(seq 0 2 30); do
        old="$root/tree/d$dir/f$file"
        new="$root/tree/d$dir/.apk-new-$file"
        mv "$old" "$new"
        printf 'replacement=%02d/%02d\n' "$dir" "$file" >> "$new"
        mv "$new" "$old"
    done
    printf 'EXT4_APK_REPRO_RENAME_DIR=%d DONE\n' "$dir"
done
printf 'EXT4_APK_REPRO_RENAME=OK files=256\n'

# The next command is the first truncate process started after the backend
# object cache has churned past its capacity.  Verify the executable and its
# contents before exec so a stale cross-inode page-cache read is reported as
# data corruption instead of surfacing later as an opaque fault.
test "$(/usr/bin/sha256sum /usr/bin/truncate | while read -r sum _; do printf '%s' "$sum"; done)" = "$sync_sha256"
printf 'EXT4_APK_REPRO_IMMUTABLE_AFTER_RENAME=OK\n'

for dir in $(seq 0 15); do
    printf 'EXT4_APK_REPRO_MUTATE_DIR=%d BEGIN\n' "$dir"
    for file in $(seq 1 4 29); do
        truncate -s "$((file * 137))" "$root/tree/d$dir/f$file"
    done
    rm_observed=$(/usr/bin/sha256sum /bin/rm | while read -r sum _; do printf '%s' "$sum"; done)
    printf 'EXT4_APK_REPRO_RM_HASH dir=%d sha256=%s expected=%s\n' \
        "$dir" "$rm_observed" "$sync_sha256"
    test "$rm_observed" = "$sync_sha256"
    printf 'EXT4_APK_REPRO_MUTATE_DIR=%d TRUNCATE_DONE\n' "$dir"
    for file in $(seq 3 4 31); do
        rm "$root/tree/d$dir/f$file"
    done
    printf 'EXT4_APK_REPRO_MUTATE_DIR=%d DONE\n' "$dir"
done
printf 'EXT4_APK_REPRO_TRUNCATE_UNLINK=OK files=256\n'

open_unlink="$root/open-unlink"
exec 9> "$open_unlink"
printf 'before-unlink\n' >&9
rm "$open_unlink"
printf 'after-unlink\n' >&9
exec 9>&-
test ! -e "$open_unlink"
printf 'EXT4_APK_REPRO_OPEN_UNLINK=OK\n'

pids=
for worker in 0 1 2 3; do
    (
        printf 'EXT4_APK_REPRO_WORKER=%d START\n' "$worker"
        for file in $(seq 0 63); do
            if [ "$((file % 8))" -eq 0 ]; then
                printf 'EXT4_APK_REPRO_WORKER=%d FILE=%d BEGIN\n' "$worker" "$file"
            fi
            path="$root/parallel/w${worker}-$file"
            printf 'worker=%d file=%d\n' "$worker" "$file" > "$path"
            if [ "$((file % 3))" -eq 0 ]; then
                mv "$path" "$path.renamed"
            elif [ "$((file % 3))" -eq 1 ]; then
                rm "$path"
            else
                truncate -s "$((file * 97))" "$path"
            fi
            if [ "$((file % 8))" -eq 7 ]; then
                printf 'EXT4_APK_REPRO_WORKER=%d FILE=%d DONE\n' "$worker" "$file"
            fi
        done
        printf 'EXT4_APK_REPRO_WORKER=%d DONE\n' "$worker"
    ) &
    pids="$pids $!"
done
parallel_status=0
for pid in $pids; do
    if ! wait "$pid"; then
        parallel_status=1
    fi
done
test "$parallel_status" = 0
printf 'EXT4_APK_REPRO_PARALLEL=OK operations=256\n'

: > "$root/inventory"
inventory_count=0
for dir in "$root"/tree/d*; do
    for path in "$dir"/*; do
        test -f "$path" || continue
        printf '%s\n' "$path" >> "$root/inventory"
        inventory_count=$((inventory_count + 1))
    done
done
test "$inventory_count" = 384
sync_observed=$(/usr/bin/sha256sum /bin/sync | while read -r sum _; do printf '%s' "$sum"; done)
printf 'EXT4_APK_REPRO_IMMUTABLE_AFTER sha256=%s expected=%s\n' \
    "$sync_observed" "$sync_sha256"
test "$sync_observed" = "$sync_sha256"
sync -f "$root/inventory"
sync
printf 'EXT4_APK_REPRO_FSYNC=OK inventory=384\n'

rm -rf -- "$root"
sync
test ! -e "$root"
printf 'EXT4_APK_REPRO_CLEANUP=OK\n'
printf 'EXT4_APK_REPRO_DONE\n'
