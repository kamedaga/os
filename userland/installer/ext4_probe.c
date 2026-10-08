/* SPDX-License-Identifier: MIT */
#include "ext4_probe.h"

#include <errno.h>
#include <stdlib.h>

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int installer_probe_ext4(installer_read_at read_at, void *context,
    uint64_t disk_bytes, uint32_t logical_block,
    uint64_t first_lba, uint64_t last_lba) {
    if (!read_at || logical_block < 512 || logical_block > 4096 ||
        (logical_block & (logical_block - 1)) ||
        disk_bytes % logical_block || first_lba > last_lba ||
        last_lba >= disk_bytes / logical_block ||
        last_lba - first_lba < 2) return -EINVAL;

    /* The ext superblock starts 1024 bytes into the partition. Read the
     * containing logical sector instead of issuing unaligned block I/O. */
    uint64_t magic_offset = first_lba * (uint64_t)logical_block + 1024 + 56;
    uint64_t sector_start = magic_offset & ~((uint64_t)logical_block - 1);
    uint64_t relative = sector_start - first_lba * (uint64_t)logical_block;
    if (relative + logical_block >
        (last_lba - first_lba + 1) * (uint64_t)logical_block)
        return -EINVAL;
    uint8_t *sector = malloc(logical_block);
    if (!sector) return -ENOMEM;
    int result = read_at(context, sector_start, sector, logical_block);
    if (!result) {
        const uint8_t *sb = sector + 1024 - relative;
        /* A shared ext2/3/4 magic is not enough: EXTENTS is an ext4
         * incompatible feature. A full structural check remains e2fsck's
         * job before reusing a filesystem. */
        if (le16(sb + 56) != 0xef53 ||
            !(le32(sb + 96) & 0x40) ||
            le32(sb + 24) > 6 ||
            le32(sb + 4) == 0)
            result = -EINVAL;
    }
    free(sector);
    return result;
}
