#!/bin/bash
set -u

mark()
{
    printf 'EXT4_HARDLINK_%s\n' "$1"
}

fail()
{
    status=$?
    printf 'EXT4_HARDLINK_FAIL stage=%s status=%s\n' "$1" "$status"
    exit "$status"
}

if apk info -e binutils >/dev/null 2>&1; then
    apk --progress=no del binutils || fail preclean
fi

mark BEGIN
apk update || fail update
mark UPDATE=OK
apk -v add binutils || fail add
mark ADD=OK

ld_inode=$(stat -c '%i' /usr/bin/ld) || fail stat-ld
ldbfd_inode=$(stat -c '%i' /usr/bin/ld.bfd) || fail stat-ld-bfd
ld_links=$(stat -c '%h' /usr/bin/ld) || fail nlink-ld
ldbfd_links=$(stat -c '%h' /usr/bin/ld.bfd) || fail nlink-ld-bfd

test "$ld_inode" = "$ldbfd_inode" || fail inode-mismatch
test "$ld_links" -ge 2 || fail nlink-ld
test "$ldbfd_links" -ge 2 || fail nlink-ld-bfd
printf 'EXT4_HARDLINK_INODE=OK inode=%s nlink=%s\n' "$ld_inode" "$ld_links"

apk --progress=no del binutils || fail del
test ! -e /usr/bin/ld || fail ld-remains
test ! -e /usr/bin/ld.bfd || fail ld-bfd-remains
sync || fail sync
mark DEL=OK
