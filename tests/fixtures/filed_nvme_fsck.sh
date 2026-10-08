#!/bin/sh
set -eu
# These tools are installed only into Alpine's RAM root. The target is an
# offline private COW; neither apk nor e2fsck can modify its backing image.
apk add --no-network --allow-untrusted --force-non-repository /artifacts/e2fsprogs-*.apk /artifacts/libcom_err-*.apk /artifacts/libblkid-*.apk /artifacts/libuuid-*.apk /artifacts/libeconf-*.apk
e2fsck -V
printf 'NVME_FSCK_BEFORE_REPLAY_BEGIN\n'
set +e
e2fsck -f -n /dev/nvme0n1p2
before=$?
set -e
printf 'NVME_FSCK_BEFORE_REPLAY_RC=%s\n' "$before"
# PachaOS cannot unmount its active root before process termination. Replay
# only the ext4 journal, without repairing structures, in a separate overlay.
# Retain the pre-replay check as evidence rather than concealing crash state.
e2fsck -E journal_only /dev/nvme0n1p2
printf 'NVME_JOURNAL_REPLAY_ONLY=OK\n'
printf 'NVME_FSCK_FINAL_BEGIN\n'
set +e
e2fsck -f -n /dev/nvme0n1p2
after=$?
set -e
printf 'NVME_FSCK_FINAL_RC=%s\n' "$after"
test "$after" = 0
mkdir -p /pacha
mount -o ro,noload /dev/nvme0n1p2 /pacha
if [ -f /pacha/root/filed-nvme-persistence ]; then
    test "$(cat /pacha/root/filed-nvme-persistence)" = filed-nvme-synced
    printf 'NVME_PERSISTENCE=OK\n'
fi
umount /pacha
printf 'FILED_NVME_FSCK_OK\n'
