/* SPDX-License-Identifier: MIT */
#include "fat32_probe.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_sector(installer_read_at read_at, void *context,
    uint64_t first_lba, uint32_t block, uint64_t sector,
    uint8_t *buffer) {
    return read_at(context, (first_lba + sector) * (uint64_t)block,
        buffer, block);
}

int installer_probe_fat32(installer_read_at read_at, void *context,
    uint64_t disk_bytes, uint32_t logical_block,
    uint64_t first_lba, uint64_t last_lba) {
    if (!read_at || logical_block < 512 || logical_block > 4096 ||
        (logical_block & (logical_block - 1)) ||
        disk_bytes % logical_block || first_lba > last_lba ||
        last_lba >= disk_bytes / logical_block) return -EINVAL;
    const uint64_t sectors = last_lba - first_lba + 1;
    if (sectors < 65536) return -EINVAL;
    uint8_t *boot = malloc(logical_block);
    uint8_t *other = malloc(logical_block);
    if (!boot || !other) {
        free(boot); free(other); return -ENOMEM;
    }
    int status = read_sector(read_at, context, first_lba,
        logical_block, 0, boot);
    if (status) goto out;
    const uint32_t total = le32(boot + 32);
    const uint32_t fat_sectors = le32(boot + 36);
    const uint16_t reserved = le16(boot + 14);
    const uint16_t fsinfo = le16(boot + 48);
    const uint16_t backup = le16(boot + 50);
    const uint8_t cluster_sectors = boot[13];
    const uint8_t fats = boot[16];
    const uint64_t overhead = (uint64_t)reserved +
        (uint64_t)fats * fat_sectors;
    /* A pre-existing ESP must fit wholly inside the selected GPT extent.
     * Reject contradictory BPB fields before the installer lets mtools
     * write through this partition's offset. */
    if (boot[510] != 0x55 || boot[511] != 0xaa ||
        le16(boot + 11) != logical_block || !cluster_sectors ||
        (cluster_sectors & (cluster_sectors - 1)) ||
        cluster_sectors > 128 || (fats != 1 && fats != 2) ||
        reserved < 2 || !fat_sectors || !total || total > sectors ||
        overhead >= total || le16(boot + 17) || le16(boot + 22) ||
        le16(boot + 19) || le32(boot + 44) < 2 ||
        fsinfo == 0 || fsinfo >= reserved ||
        backup == 0 || backup >= reserved || backup == fsinfo ||
        (total - overhead) / cluster_sectors < 65525 ||
        memcmp(boot + 82, "FAT32", 5)) {
        status = -EINVAL; goto out;
    }
    status = read_sector(read_at, context, first_lba,
        logical_block, fsinfo, other);
    if (status) goto out;
    if (le32(other) != 0x41615252 ||
        le32(other + 484) != 0x61417272 ||
        other[510] != 0x55 || other[511] != 0xaa) {
        status = -EINVAL; goto out;
    }
    status = read_sector(read_at, context, first_lba,
        logical_block, backup, other);
    if (status) goto out;
    if (memcmp(other + 11, boot + 11, 79) ||
        other[510] != 0x55 || other[511] != 0xaa)
        status = -EINVAL;
out:
    free(other); free(boot);
    return status;
}
